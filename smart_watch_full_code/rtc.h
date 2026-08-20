#ifndef RTC_H
#define RTC_H

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>

// NTP server details
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;  // 5.5 hours offset for IST
const int daylightOffset_sec = 0;  // No Daylight Savings

RTC_DS1307 rtc; 

char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

struct tm timeinfo;
uint32_t lastSyncUnixTime = 0;  // Storing as unix time integer

// 1. Data Structure
struct RTC_Data {
    uint32_t unixTime; 
    int hour;
    int minute;
    int second;
    int day;
    int month;
    int year;
};

//Initialising the sensor
inline bool init_RTC() {
  if (!rtc.begin(&Wire)) {
    Serial.println("Couldn't find RTC");
    return false;
  }
  Serial.println("RTC connected");
  return true;
}

// 3. Read Function
inline RTC_Data readRTC() {
  RTC_Data data;
  DateTime now = rtc.now(); 
    
  data.unixTime = now.unixtime(); 
  data.hour = now.hour();
  data.minute = now.minute();
  data.second = now.second();
  data.day = now.day();
  data.month = now.month();
  data.year = now.year();
    
  return data;
}

// 4. Sync Function (Call ONLY when WiFi is connected)
inline void syncTimeRTC() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); 
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println("\nESP32 Time synchronized with NTP server.");
  
  // Sync the RTC with the NTP time
  rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));

  // Record the exact time we synced
  DateTime updatedTime = rtc.now();
  lastSyncUnixTime = updatedTime.unixtime(); 
}

#endif






//Original Example Code(TESTED and WORKING)
/*
  Rui Santos & Sara Santos - Random Nerd Tutorials
  Complete project details at https://RandomNerdTutorials.com/esp32-ds1307-real-time-clock-rtc-arduino/
  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files.  
  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
*/
//The clock synces with NTP every 1 hour
/*
#include <WiFi.h>
#include <time.h>
#include <Wire.h>      // Added Wire library for I2C communication
#include <RTClib.h>

// Define custom I2C pins for ESP32-S3
#define I2C_SDA 11
#define I2C_SCL 12

// Enter your Wi-Fi credentials
const char* ssid = "PHENIX-AX10";
const char* password = "03323359065";

// NTP server details
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;  // Offset for GMT in seconds
const int daylightOffset_sec = 19800;  // Daylight savings time in seconds

// RTC object (for DS1307 or DS3231)
RTC_DS1307 rtc;  // Change to RTC_DS1307 for DS1307 module

char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Global timeinfo struct and last sync timestamp
struct tm timeinfo;
unsigned long lastSyncMillis = 0;  // Last sync time in milliseconds

void setup() {
  Serial.begin(115200);
  setupWiFi();

  // Initialize I2C with custom ESP32-S3 pins
  Wire.begin(I2C_SDA, I2C_SCL);

  // Initialize RTC using the explicitly configured Wire
  if (!rtc.begin(&Wire)) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  // Sync the RTC at startup
  syncTime();
}

void loop() {
  checkTimeAndSync();  // Check if 1 hour has passed and sync if necessary

  // Get current time from RTC
  DateTime now = rtc.now();

  // Getting each time field in individual variables
  String yearStr = String(now.year(), DEC);
  String monthStr = (now.month() < 10 ? "0" : "") + String(now.month(), DEC);
  String dayStr = (now.day() < 10 ? "0" : "") + String(now.day(), DEC);
  String hourStr = (now.hour() < 10 ? "0" : "") + String(now.hour(), DEC);
  String minuteStr = (now.minute() < 10 ? "0" : "") + String(now.minute(), DEC);
  String secondStr = (now.second() < 10 ? "0" : "") + String(now.second(), DEC);
  String dayOfWeek = daysOfTheWeek[now.dayOfTheWeek()];

  // Complete time string
  String formattedTime = dayOfWeek + ", " + yearStr + "-" + monthStr + "-" + dayStr + " " + hourStr + ":" + minuteStr + ":" + secondStr;

  // Print the complete formatted time
  Serial.println(formattedTime);

  Serial.println();
  delay(10000);
}

void setupWiFi() {
    WiFi.begin(ssid, password);  // Connect to WiFi
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("WiFi connected.");
}

void syncTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // Configure time with NTP server
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println("\nESP32 Time synchronized with NTP server.");
  Serial.print("Current time: ");
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");

  // Sync the RTC with the NTP time
  rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));

  lastSyncMillis = millis();  // Record the last sync time in milliseconds
}

void checkTimeAndSync() {
  // Check if 1 hour has passed since the last sync (1 hour = 3600000 milliseconds)
  if (millis() - lastSyncMillis >= 3600000) {
    Serial.println("Synchronizing time with NTP...");
    syncTime();
  }
}
*/