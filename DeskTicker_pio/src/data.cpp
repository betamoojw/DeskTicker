#include "data.hpp"

#include "myUtils.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <ESP32Time.h>
#include <FS.h>
#include <SD.h>
#include <DatabaseOnSD.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "esp_task_wdt.h"

#include "esp_log.h"
#include "esp32-hal-log.h"

#define PRINT_HEADERS false
#define SC_Request_Delay 2000
#define FAIL_COUNT_LIMIT 5

static const char *myTAG = "data.cpp";

static const String DATA_DIRECTORY = "/data";

String csvData;

ushort updateInterval = 5; // in minutes
bool eodUpdate = false;
bool gotEOD = false;
bool gotNightlyCheck = false;
ushort failCount = 0;

const char *scURLBase = "stockcharts.com";
const char *scHistURLPath = "/quotebrain/history/d"; // d = daily, 5 = 5min
const char *scTodayURLPath = "/quotebrain/quote";
const char *userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0";

/***********************************************************************/
/***************************  FreeRTOS task  ***************************/
/***********************************************************************/
void dataTask(void *parameters)
{
    csvData.reserve(1024 * 10); // reserve 10KB for CSV data
    csvData.clear();

    // check if the market is open
    isMarketOpen();

    unsigned long lastUpdate = 0;

    // ticker list should be loaded by UI task first, if not load it now
    if (numTickers == 0)
    {
        loadTickers();
    }

    // check if csv files are up to date and get todays price
    for (int i = 0; i < numTickers; i++)
    {
        esp_task_wdt_reset();
        if (!isCsvFileDataUpToDate(tickerList[i].symbol))
        {
            ESP_LOGI(myTAG, "%s.csv is outdated. Updating now...", tickerList[i].symbol);
            updateCsvFile(i);
            esp_task_wdt_reset();
            vTaskDelay(SC_Request_Delay);
        }
        getTodayData(i);
        esp_task_wdt_reset();
        vTaskDelay(SC_Request_Delay);
    }
    lastUpdate = millis();

    while (1)
    {
        esp_task_wdt_reset();
        // check if the market is open
        isMarketOpen();

        // updates data if ticker list is changed from web page
        if (updateTickerList)
        {
            ESP_LOGD(myTAG, "Updating ticker list and data");
            dataDirUpdate();
            for (int i = 0; i < numTickers; i++)
            {
                esp_task_wdt_reset();
                getTodayData(i);
                vTaskDelay(SC_Request_Delay);
            }
            updateTickerList = false;
        }

        // historical data length was changed on webpage so update csv files
        if (updateHistLength)
        {
            ESP_LOGD(myTAG, "Updating CSV files with new historic data length");
            updateAllCsvFiles();
            updateHistLength = false;
        }

        if (marketOpen)
        {
            // updates the current price every <updateInterval> minutes
            if (millis() - lastUpdate > updateInterval * 60000 || millis() < lastUpdate)
            {
                for (int i = 0; i < numTickers; i++)
                {
                    esp_task_wdt_reset();
                    getTodayData(i);
                    vTaskDelay(SC_Request_Delay);
                }
                lastUpdate = millis();
            }
        }
        else if (eodUpdate)
        {
            // update csv files at the end of the day
            updateAllCsvFiles();
            eodUpdate = false;
            gotEOD = true;
        }

        // check if any data downloads needs to be retried
        checkRetryFlags();

        vTaskDelay(15000);
    }
}

/***********************************************************************/
/*************************  Helper Functions  **************************/
/***********************************************************************/

// Function to synce csv files on SD card with ticker list
void dataDirUpdate(void)
{
    loadTickers();
    xSemaphoreTake(SDmutex, portMAX_DELAY);

    // check if data directory exists and create it if not
    if (!SD.exists(DATA_DIRECTORY))
    {
        Serial.println("Data files directory does not exist. Creating it now.");
        SD.mkdir(DATA_DIRECTORY);
    }
    // open the data directory
    File dir = SD.open(DATA_DIRECTORY);
    if (!dir)
    {
        xSemaphoreGive(SDmutex);
        ESP_LOGE(myTAG, "Could not open %s directory.", DATA_DIRECTORY);
        return;
    }
    // check if files exist for tickers not in ticker list and delete them
    xSemaphoreTake(TickListmutex, portMAX_DELAY);
    while (File entry = dir.openNextFile())
    {
        String filename = entry.name();
        String symbol = filename.substring(0, filename.length() - 4); // Remove the .csv extension

        // Check if this symbol is in the tickerList
        bool found = false;
        for (int i = 0; i < numTickers; i++)
        {
            if (tickerList[i].symbol == symbol)
            {
                found = true;
                break;
            }
        }
        entry.close();
        // If the symbol is not in the tickerList, delete the CSV file
        if (!found)
        {
            ESP_LOGI(myTAG, "Deleting file: %s", filename);
            SD.remove(DATA_DIRECTORY + "/" + filename);
        }
    }
    dir.rewindDirectory();

    // Check if any symbols in the tickerList do not have an associated CSV file
    for (int i = 0; i < numTickers; i++)
    {
        bool fileExists = false;

        while (File entry = dir.openNextFile())
        {
            String filename = entry.name();
            if (filename.endsWith(".csv") && filename.substring(0, filename.length() - 4) == tickerList[i].symbol)
            {
                fileExists = true;
                ESP_LOGD(myTAG, "Found CSV file for symbol: %s", tickerList[i].symbol);
                entry.close();
                break;
            }

            entry.close();
        }
        dir.rewindDirectory();
        if (!fileExists)
        {
            // If no CSV file exists for this symbol, call getData()
            ESP_LOGI(myTAG, "No CSV found for symbol: %s", tickerList[i].symbol);
            csvData.clear();
            csvData = getHistoricData(i, priceHistLen);
            if (csvData != "")
            {
                File file = SD.open(DATA_DIRECTORY + "/" + tickerList[i].symbol + ".csv", FILE_WRITE);
                if (!file)
                {
                    ESP_LOGW(myTAG, "Failed to open file for writing");
                }
                else
                {
                    // Write the received CSV data to the file
                    file.print(csvData);
                    file.close();
                    ESP_LOGI(myTAG, "Data saved to %s/%s.csv", DATA_DIRECTORY, tickerList[i].symbol);
                }
                csvData.clear();
            }
            vTaskDelay(SC_Request_Delay); // Delay to avoid sending too many requests at once
        }
    }
    dir.close();
    xSemaphoreGive(TickListmutex);
    xSemaphoreGive(SDmutex);
}

// Function to calculate the date N days ago in YYYYMMDD format
String getStartDate(const int length)
{
    // Get the current time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Subtract N days (30 * 24 * 60 * 60 seconds)
    now -= (length * 24 * 60 * 60);
    localtime_r(&now, &timeinfo);

    // Format the date as YYYYMMDD
    char dateBuffer[9];
    snprintf(dateBuffer, sizeof(dateBuffer), "%04d%02d%02d",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday);
    ESP_LOGV(myTAG, "Start date: %s", String(dateBuffer));
    return String(dateBuffer);
}

// Function to get price data from StockCharts.com and save to CSV file
String getHistoricData(const int tickerNum, const int length)
{
    bool success = false;
    String symbol = tickerList[tickerNum].symbol;
    ESP_LOGV(myTAG, "Downloading data for %s, length = %d", symbol, length);
    for (int j = 0; j < 2; j++)
    {
        esp_task_wdt_reset();
        if (j > 0)
        {
            ESP_LOGW(myTAG, "Attempt %d failed to get historic data for %s", j, symbol);
            ESP_LOGD(myTAG, "Waiting %ds before retry...", SC_Request_Delay * j / 1000);
        }
        vTaskDelay(SC_Request_Delay * j);
        xSemaphoreTake(Clientmutex, portMAX_DELAY);
        WiFiClientSecure localClient;
        localClient.setInsecure();
        localClient.setTimeout(30);
        localClient.setHandshakeTimeout(20);
        localClient.connect(scURLBase, 443);
        vTaskDelay(150);
        if (!localClient.connected())
        {
            char err_buf[100];
            int error_code = localClient.lastError(err_buf, 100);
            if (error_code < 0)
            {
                ESP_LOGW(myTAG, "SC connect failed: %d - %s", error_code, err_buf);
            }
            else
            {
                ESP_LOGW(myTAG, "SC connection error");
            }
            localClient.stop();
            xSemaphoreGive(Clientmutex);
            continue;
        }

        //  build query string
        String queryParams = String("?symbol=") + symbol + "&start=" + getStartDate(length);

        // Construct the HTTP request
        String request = String("GET ") + scHistURLPath + queryParams + " HTTP/1.0\r\n" +
                         "Host: " + scURLBase + "\r\n" +
                         "User-Agent: " + userAgent + "\r\n" +
                         "Accept: application/json, text/plain, */*\r\n" +
                         "Accept-Encoding: identity\r\n" +
                         "Connection: close\r\n\r\n";

        // Send the request
        localClient.print(request);

        // Wait for response
        while (localClient.connected() && !localClient.available())
        {
            vTaskDelay(25);
        }

        // read and validate HTTP status line
        String statusLine = localClient.readStringUntil('\n');
        if (!statusLine.startsWith("HTTP/1.1 2"))
        {
            ESP_LOGW(myTAG, "HTTP Error: %s", statusLine.c_str());
            Serial.println(statusLine);
            localClient.stop();
            xSemaphoreGive(Clientmutex);
            continue;
        }
        ESP_LOGD(myTAG, "Historical Attempt: %d, Ticker: %s, HTTP Status: %s", j, symbol, statusLine.c_str());

        // header parsing
        bool headersComplete = false;
        while (localClient.connected() && !headersComplete)
        {
            if (localClient.available())
            {
                String line = localClient.readStringUntil('\n');
                if (PRINT_HEADERS)
                {
                    Serial.println(line);
                }
                line.trim(); // Remove whitespace/carriage returns
                if (line.length() == 0)
                {
                    headersComplete = true;
                }
            }
            else
            {
                delay(15);
            }
        }

        // check if data is available
        if (!localClient.available())
        {
            ESP_LOGW(myTAG, "No response body available");
            localClient.stop();
            xSemaphoreGive(Clientmutex);
            continue;
        }

        // Create a filter to extract the desired fields
        JsonDocument filter;
        filter["intervals"][0]["end"]["time"] = true;
        filter["intervals"][0]["close"] = true;
        // Parse the JSON response
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, localClient, DeserializationOption::Filter(filter));

        localClient.stop();
        xSemaphoreGive(Clientmutex);

        if (error)
        {
            ESP_LOGW(myTAG, "Failed to parse JSON: %s", error.c_str());
            continue;
        }
        // Print the JSON document
        // serializeJsonPretty(doc, Serial);

        // parse data to CSV
        csvData.clear();
        csvData += "Date,Close\n";
        JsonArray intervals = doc["intervals"];
        for (JsonObject interval : intervals)
        {
            csvData += interval["end"]["time"].as<String>().substring(0, 10) + "," + interval["close"].as<String>() + "\n";
        }
        success = true;
        // Serial.println("CSV Data:");
        // Serial.println(csvData);
        break;
    }
    if (!success)
    {
        tickerList[tickerNum].csvRetry = true;
        ESP_LOGW(myTAG, "Failed to get historic data for %s", symbol);
        failCount++;
        if (failCount > FAIL_COUNT_LIMIT)
        {
            ESP_LOGE(myTAG, "Too many failed attempts to get data. Rebooting...");
            reboot();
        }
        return "";
    }

    return csvData;
}

// Function to get todays price data from StockCharts.com
void getTodayData(const int tickerNum)
{
    bool success = false;
    xSemaphoreTake(TickListmutex, portMAX_DELAY);
    ESP_LOGV(myTAG, "Getting todays price for %s", tickerList[tickerNum].symbol);
    for (int jj = 0; jj < 2; jj++)
    {
        esp_task_wdt_reset();
        if (jj > 0)
        {
            ESP_LOGW(myTAG, "Attempt %d to get todays price for %s failed", jj, tickerList[tickerNum].symbol);
            ESP_LOGD(myTAG, "Waiting %ds before retry...", SC_Request_Delay * jj / 1000);
        }
        vTaskDelay(SC_Request_Delay * jj);
        xSemaphoreTake(Clientmutex, portMAX_DELAY);
        WiFiClientSecure localClient2;
        localClient2.setInsecure();
        localClient2.setTimeout(30);
        localClient2.setHandshakeTimeout(20);
        localClient2.connect(scURLBase, 443);
        vTaskDelay(150);
        if (!localClient2.connected())
        {
            char err_buf[100];
            int error_code = localClient2.lastError(err_buf, 100);
            if (error_code < 0)
            {
                ESP_LOGW(myTAG, "SC connect failed: %d - %s", error_code, err_buf);
            }
            else
            {
                ESP_LOGW(myTAG, "SC connection error");
            }
            localClient2.stop();
            xSemaphoreGive(Clientmutex);
            continue;
        }

        //  build query string
        String queryParams = String("?symbol=") + tickerList[tickerNum].symbol + "&format=json";

        // Construct the HTTP request
        String request = String("GET ") + scTodayURLPath + queryParams + " HTTP/1.0\r\n" +
                         "Host: " + scURLBase + "\r\n" +
                         "User-Agent: " + userAgent + "\r\n" +
                         "Accept: application/json, text/plain, */*\r\n" +
                         "Accept-Encoding: identity\r\n" +
                         "Connection: close\r\n\r\n";

        // Send the request
        localClient2.print(request);

        // Wait for response
        while (localClient2.connected() && !localClient2.available())
        {
            vTaskDelay(25);
        }

        // read and validate HTTP status line
        String statusLine = localClient2.readStringUntil('\n');
        if (!statusLine.startsWith("HTTP/1.1 2"))
        {
            ESP_LOGW(myTAG, "HTTP Error: %s", statusLine.c_str());
            localClient2.stop();
            xSemaphoreGive(Clientmutex);
            continue;
        }
        ESP_LOGD(myTAG, "Today Attempt: %d, Ticker: %s, HTTP Status: %s", jj, tickerList[tickerNum].symbol, statusLine.c_str());

        // header parsing
        bool headersComplete = false;
        while (localClient2.connected() && !headersComplete)
        {
            if (localClient2.available())
            {
                String line = localClient2.readStringUntil('\n');
                if (PRINT_HEADERS)
                {
                    Serial.println(line);
                }
                line.trim();
                if (line.length() == 0)
                {
                    headersComplete = true; // Empty line indicates end of headers
                }
            }
            else
            {
                delay(15);
            }
        }

        // check if data is available
        if (!localClient2.available())
        {
            ESP_LOGW(myTAG, "No response body available");
            localClient2.stop();
            xSemaphoreGive(Clientmutex);
            continue;
        }

        // Create a filter to extract the desired fields
        JsonDocument filter;
        filter[0]["close"] = true;
        filter[0]["closeYesterday"] = true;
        // Parse the JSON response
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, localClient2, DeserializationOption::Filter(filter));

        localClient2.stop();
        xSemaphoreGive(Clientmutex);

        if (error)
        {
            ESP_LOGW(myTAG, "Failed to parse JSON: %s", error.c_str());
            continue;
        }
        // Print the JSON document
        // serializeJson(doc, Serial);

        float todaysClose = doc[0]["close"].as<float>();
        float CloseYesterday = doc[0]["closeYesterday"].as<float>();
        if (todaysClose != 0 && CloseYesterday != 0)
        {
            tickerList[tickerNum].price = todaysClose;
            tickerList[tickerNum].change = todaysClose - CloseYesterday;
            tickerList[tickerNum].changePct = (tickerList[tickerNum].change / CloseYesterday) * 100;
            ESP_LOGD(myTAG, "Ticker: %s, Price: %.2f, Change: %.2f, ChangePct: %.2f%%", tickerList[tickerNum].symbol.c_str(), tickerList[tickerNum].price, tickerList[tickerNum].change, tickerList[tickerNum].changePct);
            success = true;
        }
        else
        {
            continue;
        }
        break;
    }
    xSemaphoreGive(TickListmutex);

    if (!success)
    {
        tickerList[tickerNum].curPricRetry = true;
        ESP_LOGW(myTAG, "Failed to get todays price for %s", tickerList[tickerNum].symbol);
        failCount++;
        if (failCount > FAIL_COUNT_LIMIT)
        {
            ESP_LOGE(myTAG, "Too many failed attempts to get data. Rebooting...");
            reboot();
        }
    }
    else
    {
        tickerList[tickerNum].curPricRetry = false;
    }
}

// Function to check if market is open
void isMarketOpen(void)
{
    const int dow = rtc.getDayofWeek();
    const int h = rtc.getHour(true);
    const int m = rtc.getMinute();

    if (dow >= 1 && dow <= 5)
    {
        if (h == 9 && m >= 30)
        {
            marketOpen = true;
            gotEOD = false;
            gotNightlyCheck = false;
        }
        else if (h == 16 && m <= 15)
        {
            marketOpen = true;
        }
        else if (h == 16 && m > 15 and !gotEOD)
        {
            marketOpen = false;
            eodUpdate = true;
        }
        else if (h > 9 && h < 16)
        {
            marketOpen = true;
        }
        else if (h == 23 && !gotNightlyCheck)
        {
            failCount = 0;
            for (int i = 0; i < numTickers; i++)
            {
                if (!isCsvFileDataUpToDate(tickerList[i].symbol))
                {
                    ESP_LOGI(myTAG, "%s.csv is outdated. Updating now...", tickerList[i].symbol);
                    updateCsvFile(i);
                    vTaskDelay(SC_Request_Delay);
                }
            }
            gotNightlyCheck = true;
        }
        else
        {
            marketOpen = false;
        }
    }
    else
    {
        marketOpen = false;
    }
}

// Function to update data csv files at end of day
void updateAllCsvFiles(void)
{
    ESP_LOGI(myTAG, "Updating all CSV data files");
    for (int i = 0; i < numTickers; i++)
    {
        esp_task_wdt_reset();
        updateCsvFile(i);
        vTaskDelay(SC_Request_Delay); // Delay to avoid sending too many requests
    }
}

// Function to update a CSV data file
bool updateCsvFile(const int tickerIndex)
{
    bool success = false;

    String symbol = tickerList[tickerIndex].symbol;

    csvData.clear();
    ESP_LOGD(myTAG, "Updating %s.csv l=%d", symbol, priceHistLen);
    csvData = getHistoricData(tickerIndex, priceHistLen);
    if (csvData != "")
    {
        File file = SD.open(DATA_DIRECTORY + "/" + symbol + ".csv", FILE_WRITE);
        if (!file)
        {
            ESP_LOGW(myTAG, "Failed to open %s/%s.csv for writing", DATA_DIRECTORY, symbol);
        }
        else
        {
            // Write the received CSV data to the file
            file.print(csvData);
            file.close();
            ESP_LOGI(myTAG, "Data successfully saved to %s/%s.csv", DATA_DIRECTORY, symbol);
            success = true;
        }
        csvData.clear();
    }

    return success;
}

// Function to check if CSV data files are up todate
bool isCsvFileDataUpToDate(const String &symbol)
{
    MyTable table(DATA_DIRECTORY + "/" + symbol + ".csv");
    bool dataUpToDate = false;
    int rows = 0;
    rows = table.countRows();
    String lastDate;

    if (rows != 0)
    {
        lastDate = table.readCell(rows - 1, 0);
        int month = lastDate.substring(5, 7).toInt();
        int day = lastDate.substring(8, 10).toInt();
        ESP_LOGD(myTAG, "Last date in %s.csv: %d/%d", symbol, month, day);
        // if current month
        if (month == rtc.getMonth() + 1)
        {
            // if current day
            if (day == rtc.getDay())
            {
                dataUpToDate = true;
            }
            // if data is from yesterday and market is open (to prevent trying to update end of day only data)
            else if (abs(rtc.getDay() - day) < 2 && marketOpen)
            {
                dataUpToDate = true;
            }
        }
        // if it is a weekend
        else if (rtc.getDayofWeek() == 0 || rtc.getDayofWeek() == 6)
        {
            // if data is from last friday
            if (abs(rtc.getDay() - day) < 3 || rtc.getDay() <= 2)
            {
                dataUpToDate = true;
            }
        }
    }

    return dataUpToDate;
}

// function to check for any retry flags in tickerList and retry getting data
void checkRetryFlags(void)
{
    // Check all tickers
    for (int i = 0; i < numTickers; i++)
    {
        // If ticker retry fails more than 3 times, only retry every 10th time
        if (tickerList[i].RetryCount < 3 || tickerList[i].RetryCount % 10 == 0)
        {
            if (tickerList[i].curPricRetry)
            {
                ESP_LOGW(myTAG, "[Out of norm] Retrying to get current price for %s", tickerList[i].symbol);
                getTodayData(i);
                vTaskDelay(SC_Request_Delay);
            }

            if (tickerList[i].csvRetry)
            {
                ESP_LOGW(myTAG, "[Out of norm] Retrying to update %s.csv", tickerList[i].symbol);
                if (updateCsvFile(i))
                {
                    tickerList[i].csvRetry = false;
                    tickerList[i].RetryCount = 0;
                }
                else
                {
                    tickerList[i].RetryCount++;
                }
                vTaskDelay(SC_Request_Delay);
            }
        }
        else
        {
            tickerList[i].RetryCount++;
        }
    }
}