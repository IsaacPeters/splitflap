/*
   Copyright 2021 Scott Bezek and the splitflap contributors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#define HTTP true
#if HTTP
#include "http_task.h"

#include <HTTPClient.h>
#include <lwip/apps/sntp.h>
#include <json11.hpp>
#include <time.h>
#include <ArduinoOTA.h>

#include "secrets.h"

using namespace json11;

// About this example:
// - Fetches current weather data for an area in San Francisco (updating infrequently)
// - Cycles between showing the temperature and the wind speed on the split-flaps (cycles frequently)
//
// Make sure to set up secrets.h - see secrets.h.example for more.
//
// What this example demonstrates:
// - a simple JSON GET request (see fetchData)
// - json response parsing using json11 (see handleData)
// - cycling through messages at a different interval than data is loaded (see run)

// Update data every 10 minutes
#define REQUEST_INTERVAL_MILLIS (10 * 60 * 1000)

// Cycle the message that's showing more frequently, every 30 seconds (exaggerated for example purposes)
#define MESSAGE_CYCLE_INTERVAL_MILLIS (5 * 1000)
#define MESSAGE_DURATION (5 * 1000)

// Don't show stale data if it's been too long since successful data load
#define STALE_TIME_MILLIS (REQUEST_INTERVAL_MILLIS * 3)

// Public token for synoptic data api (it's not secret, but please don't abuse it)
#define SYNOPTICDATA_TOKEN "e763d68537d9498a90fa808eb9d415d9"

// Timezone for local time strings; this is America/Los_Angeles. See https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define TIMEZONE "PST8PDT,M3.2.0,M11.1.0"

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 3600;

bool HTTPTask::fetchData() {
    char buf[200];
    uint32_t start = millis();
    HTTPClient http;

    // Construct the http request
    http.begin("https://api.synopticdata.com/v2/stations/latest?&token=" SYNOPTICDATA_TOKEN "&within=30&units=english&vars=air_temp,wind_speed&varsoperator=and&radius=45.5061697,-122.6235114,4&limit=20&fields=stid");

    // If you wanted to add headers, you would do so like this:
    // http.addHeader("Accept", "application/json");

    // Send the request as a GET
    logger_.log("Sending request");
    int http_code = http.GET();

    snprintf(buf, sizeof(buf), "Finished request in %lu millis.", millis() - start);
    logger_.log(buf);
    if (http_code > 0) {
        String data = http.getString();
        http.end();

        snprintf(buf, sizeof(buf), "Response code: %d Data length: %d", http_code, data.length());
        logger_.log(buf);

        std::string err;
        Json json = Json::parse(data.c_str(), err);

        if (err.empty()) {
            return handleData(json);
        } else {
            snprintf(buf, sizeof(buf), "Error parsing response! %s", err.c_str());
            logger_.log(buf);
            return false;
        }
    } else {
        snprintf(buf, sizeof(buf), "Error on HTTP request (%d): %s", http_code, http.errorToString(http_code).c_str());
        logger_.log(buf);
        http.end();
        return false;
    }
}

bool HTTPTask::handleData(Json json) {
    // Extract data from the json response. You could use ArduinoJson, but I find json11 to be much
    // easier to use albeit not optimized for a microcontroller.

    // Example data:
    /*
        {
            ...
            "STATION": [
                {
                    "STID": "F4637",
                    "OBSERVATIONS": {
                        "wind_speed_value_1": {
                            "date_time": "2021-11-30T23:25:00Z",
                            "value": 0.87
                        },
                        "air_temp_value_1": {
                            "date_time": "2021-11-30T23:25:00Z",
                            "value": 69
                        }
                    },
                    ...
                },
                {
                    "STID": "C5988",
                    "OBSERVATIONS": {
                        "wind_speed_value_1": {
                            "date_time": "2021-11-30T23:24:00Z",
                            "value": 1.74
                        },
                        "air_temp_value_1": {
                            "date_time": "2021-11-30T23:24:00Z",
                            "value": 68
                        }
                    },
                    ...
                },
                ...
            ]
        }
    */

   // Validate json structure and extract data:
    auto station = json["STATION"];
    if (!station.is_array()) {
        logger_.log("Parse error: STATION");
        return false;
    }
    auto station_array = station.array_items();

    std::vector<double> temps;
    std::vector<double> wind_speeds;

    for (uint8_t i = 0; i < station_array.size(); i++) {
        auto item = station_array[i];
        if (!item.is_object()) {
            logger_.log("Bad station item, ignoring");
            continue;
        }
        auto observations = item["OBSERVATIONS"];
        if (!observations.is_object()) {
            logger_.log("Bad station observations, ignoring");
            continue;
        }

        auto air_temp_value = observations["air_temp_value_1"];
        if (!air_temp_value.is_object()) {
            logger_.log("Bad air_temp_value_1, ignoring");
            continue;
        }
        auto value = air_temp_value["value"];
        if (!value.is_number()) {
            logger_.log("Bad air temp, ignoring");
            continue;
        }
        temps.push_back(value.number_value());

        auto wind_speed_value = observations["wind_speed_value_1"];
        if (!wind_speed_value.is_object()) {
            logger_.log("Bad wind_speed_value_1, ignoring");
            continue;
        }
        value = wind_speed_value["value"];
        if (!value.is_number()) {
            logger_.log("Bad wind speed, ignoring");
            continue;
        }
        wind_speeds.push_back(value.number_value());
    }

    auto entries = temps.size();
    if (entries == 0) {
        logger_.log("No data found");
        return false;
    }

    // Calculate medians
    std::sort(temps.begin(), temps.end());
    std::sort(wind_speeds.begin(), wind_speeds.end());
    double median_temp;
    double median_wind_speed;
    if ((entries % 2) == 0) {
        median_temp = (temps[entries/2 - 1] + temps[entries/2]) / 2;
        median_wind_speed = (wind_speeds[entries/2 - 1] + wind_speeds[entries/2]) / 2;
    } else {
        median_temp = temps[entries/2];
        median_wind_speed = wind_speeds[entries/2];
    }

    char buf[200];
    snprintf(buf, sizeof(buf), "Medians from %d stations: temp=%dºF, wind speed=%d knots", entries, (int)median_temp, (int)median_wind_speed);
    logger_.log(buf);

    // Construct the messages to display
    messages_.empty();

    snprintf(buf, sizeof(buf), "%d f", (int)median_temp);
    messages_.push(String(buf));

    snprintf(buf, sizeof(buf), "%dmph", (int)(median_wind_speed * 1.151));
    messages_.push(String(buf));

    // Show the data fetch time on the LCD
    time_t now;
    time(&now);
    strftime(buf, sizeof(buf), "Data: %Y-%m-%d %H:%M:%S", localtime(&now));
    display_task_.setMessage(0, String(buf));
    return true;
}

bool HTTPTask::addStockPriceToMessages(String symbol) {
    HTTPClient http;

    http.begin("https://www.alphavantage.co/query?function=GLOBAL_QUOTE&symbol=" + symbol + "&apikey=" + ALPHAVANTAGE_TOKEN);
    symbol.toLowerCase();
    messages_.push(symbol);
    int httpCode = http.GET();

    if (httpCode > 0) {
        String payload = http.getString();
        // int index1 = payload.indexOf("\"05. price\": ") + 13;
        // int index2 = payload.indexOf("}");
        // String price = payload.substring(index1, index2 - 1);
        // price.trim();
        // float priceNum = price.toFloat();

        std::string err;
        Json json = Json::parse(payload.c_str(), err);

        if (!err.empty()) {
            char buf[200];
            snprintf(buf, sizeof(buf), "Error parsing response! %s", err.c_str());
            logger_.log(buf);
            return false;
        }

        auto quote = json["Global Quote"];
        if (!quote.is_object()) {
            logger_.log("Parse error: [Global Quote]");
            return false;
        }
        auto queryPrice = quote["05. price"];
        if (!queryPrice.is_string()) {
            logger_.log("Parse error: [05. price]");
            return false;
        }
        String price = queryPrice.string_value().c_str();
        price.trim();
        double priceNum = price.toFloat();

        char strPrice[6] = " big ";
        if (priceNum < 100)
        {
            dtostrf(priceNum, 5, 2, strPrice);
        }
        else if (priceNum < 1000)
        {
            dtostrf(priceNum, 5, 1, strPrice);
        }
        else if (priceNum < 100000)
        {
            dtostrf(priceNum, 5, 0, strPrice);
        }
        messages_.push(strPrice);
    } else {
        logger_.log("Error getting stock price");
    }

    http.end();
    return true;
}


HTTPTask::HTTPTask(SplitflapTask& splitflap_task, DisplayTask& display_task, Logger& logger, const uint8_t task_core) :
        Task("HTTP", 8192, 1, task_core),
        splitflap_task_(splitflap_task),
        display_task_(display_task),
        logger_(logger),
        wifi_client_() {
}

void HTTPTask::connectWifi() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    char buf[256];

    logger_.log("Establishing connection to WiFi..");
    snprintf(buf, sizeof(buf), "Wifi connecting to %s", WIFI_SSID);
    display_task_.setMessage(1, String(buf));
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
    }

    snprintf(buf, sizeof(buf), "Connected to network %s", WIFI_SSID);
    logger_.log(buf);

    // Sync SNTP
    sntp_setoperatingmode(SNTP_OPMODE_POLL);

    char server[] = "time.nist.gov"; // sntp_setservername takes a non-const char*, so use a non-const variable to avoid warning
    sntp_setservername(0, server);
    sntp_init();

    logger_.log("Waiting for NTP time sync...");
    snprintf(buf, sizeof(buf), "Syncing NTP time via %s...", server);
    display_task_.setMessage(1, String(buf));
    time_t now;
    while (time(&now),now < 1625099485) {
        delay(1000);
    }

    setenv("TZ", TIMEZONE, 1);
    tzset();
    strftime(buf, sizeof(buf), "Got time: %Y-%m-%d %H:%M:%S", localtime(&now));
    logger_.log(buf);

    // OTA Config
    // ArduinoOTA.setPort(8266);

    ArduinoOTA
    .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
            type = "sketch";
        else // U_SPIFFS
            type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
        })
        .onEnd([]() {
        Serial.println("\nEnd");
        })
        .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        })
        .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

    ArduinoOTA.begin();
    
    logger_.log(WiFi.localIP().toString().c_str());
    logger_.log("Done with WiFi..");
    
    splitflap_task_.showString("hi...", NUM_MODULES, false);

    addStockPriceToMessages("AMZN");

    last_message_change_time_ = millis();
}

void HTTPTask::run() {
    char buf[max(NUM_MODULES + 1, 200)];

    connectWifi();

    uint32_t intraMinuteDuplicationProtection = 0;
    while(1) {
        // Handle OTA update
        ArduinoOTA.handle();

        long now = millis();
        time_t tNow;
        time(&tNow);
        // override; night should be unchanging from 9pm to 9am
        if (localtime(&tNow)->tm_hour == 21 && localtime(&tNow)->tm_min == 0)
        {
            splitflap_task_.showString("night", NUM_MODULES, false);
            splitflap_task_.disableAll();
            delay(60000);
            continue;
        }
        else if (localtime(&tNow)->tm_hour >= 21 || localtime(&tNow)->tm_hour <= 8)
        {
            // Night should have gotten displayed, now do nothing.
            delay(10000);
            continue;
        }

        if (now - intraMinuteDuplicationProtection > 60000) {
            if (localtime(&tNow)->tm_hour == 9 && localtime(&tNow)->tm_min == 0)
            {
                splitflap_task_.resetAll();
                messages_.push("wakey");
                messages_.push("wakey");
                messages_.push("eggsn");
                messages_.push("bakey");
                intraMinuteDuplicationProtection = now;
            }
            else if (localtime(&tNow)->tm_hour == 11 && localtime(&tNow)->tm_min == 35)
            {
                if (WiFi.status() != WL_CONNECTED)
                {
                    WiFi.reconnect();
                };

                if (WiFi.status() == WL_CONNECTED)
                {
                    messages_.push("symbl");
                    messages_.push("price");

                    addStockPriceToMessages("AMZN");
                    addStockPriceToMessages("VOO");
                    intraMinuteDuplicationProtection = now;
                }
            }
            else if (localtime(&tNow)->tm_hour == 12 && localtime(&tNow)->tm_min == 0)
            {
                messages_.push("it's");
                messages_.push("lunch");
                messages_.push("time");
                intraMinuteDuplicationProtection = now;
            }
            else if (localtime(&tNow)->tm_hour == 13 && localtime(&tNow)->tm_min == 0)
            {
                messages_.push("back");
                messages_.push("to");
                messages_.push("work");
                intraMinuteDuplicationProtection = now;
            }
            else if (localtime(&tNow)->tm_hour == 13 && localtime(&tNow)->tm_min == 55)
            {
                messages_.push("test");
                messages_.push("1");
                messages_.push("2");
                intraMinuteDuplicationProtection = now;
            }
            else if (localtime(&tNow)->tm_hour == 17 && localtime(&tNow)->tm_min == 0)
            {
                messages_.push("nice");
                messages_.push("work");
                intraMinuteDuplicationProtection = now;
            }
        }

        // Loop through messages if it exists every time we hit the interval
        if (messages_.size() && now - last_message_change_time_ > MESSAGE_CYCLE_INTERVAL_MILLIS) {
            String message = messages_.front();
            messages_.pop();

            snprintf(buf, sizeof(buf), "Cycling to next message: %s", message.c_str());
            logger_.log(buf);

            // Pad message for display
            size_t len = strlcpy(buf, message.c_str(), sizeof(buf));
            memset(buf + len, ' ', sizeof(buf) - len);

            splitflap_task_.showString(buf, NUM_MODULES, false);

            last_message_change_time_ = millis();
        }
        else if (now > last_message_change_time_ + MESSAGE_CYCLE_INTERVAL_MILLIS) {
            char curTime[NUM_MODULES + 3] = {0};
            strftime(curTime, sizeof(curTime), "t%H%M", localtime(&tNow));
            logger_.log(buf);

            if (localtime(&tNow)->tm_hour >= 21 || localtime(&tNow)->tm_hour <= 8)
            {
                splitflap_task_.showString("night", NUM_MODULES, false);
            }
            else if (curTime != m_lastSeenTime.c_str())
            {
                snprintf(buf, sizeof(buf), "Cycling to next message: %s", curTime);
                logger_.log(buf);
                
                splitflap_task_.showString(curTime, NUM_MODULES, false);
                m_lastSeenTime = curTime;
            }
        }

        String wifi_status;
        switch (WiFi.status()) {
            case WL_IDLE_STATUS:
                wifi_status = "Idle";
                break;
            case WL_NO_SSID_AVAIL:
                wifi_status = "No SSID";
                break;
            case WL_CONNECTED:
                wifi_status = String(WIFI_SSID) + " " + WiFi.localIP().toString();
                break;
            case WL_CONNECT_FAILED:
                wifi_status = "Connection failed";
                break;
            case WL_CONNECTION_LOST:
                wifi_status = "Connection lost";
                break;
            case WL_DISCONNECTED:
                wifi_status = "Disconnected";
                break;
            default:
                wifi_status = "Unknown";
                break;
        }
        display_task_.setMessage(1, String("Wifi: ") + wifi_status);

        delay(1000);
    }
}
#endif
