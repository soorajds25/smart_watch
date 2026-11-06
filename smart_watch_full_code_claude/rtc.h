#ifndef RTC_H
#define RTC_H

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <PCF85263A.h> 

const char* ntpServer = "pool.ntp.org";

PCF85263A rtc; 

char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

struct tm timeinfo;
uint32_t lastSyncUnixTime = 0;  

struct RTC_Data {
    uint32_t unixTime;
    int hour;
    int minute;
    int second;
    int day;
    int month;
    int year;
};

inline bool init_RTC() {
    // FIX 1: Safely set 12.5pF to fix the fast drifting time.
    // PCF85263A Oscillator Register (0x25): Bit 7 must be 1 for 12.5pF.
    // We read the existing register first so we don't corrupt the other bits.
    Wire.beginTransmission(0x51);
    Wire.write(0x25);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x51, 1) == 1) {
        uint8_t osc = Wire.read();
        osc |= 0x80; // Safely flip Bit 7 to 1
        Wire.beginTransmission(0x51);
        Wire.write(0x25);
        Wire.write(osc);
        Wire.endTransmission();
    }

    // FIX 2: Force the ESP32 environment to Indian Standard Time.
    // This Posix string natively prevents the 1-hour DST jump bug.
    setenv("TZ", "IST-5:30", 1);
    tzset();

    if (rtc.oscillator_stop()) {
        Serial.println("RTC oscillator stopped (needs NTP sync)");
    } else {
        Serial.println("RTC connected and running");
    }
    return true; 
}

inline RTC_Data readRTC() {
    RTC_Data data;
    time_t current_time = rtc.time(NULL);
    
    struct tm dt;
    
    // Because we locked the TZ environment to IST above, localtime_r 
    // will perfectly extract the time without any weird double-shifting.
    localtime_r(&current_time, &dt);

    data.unixTime = (uint32_t)current_time;
    data.hour = dt.tm_hour;
    data.minute = dt.tm_min;
    data.second = dt.tm_sec;
    data.day = dt.tm_mday;
    data.month = dt.tm_mon + 1;       
    data.year = dt.tm_year + 1900;    

    return data;
}

inline void syncTimeRTC() {
    // FIX 3: Use configTzTime instead of the buggy offset math
    configTzTime("IST-5:30", ntpServer, "time.google.com");
    
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
    }
    Serial.println("\nESP32 Time synchronized with NTP server.");

    // Write the clean, un-shifted time directly to the RTC hardware
    rtc.set(&timeinfo);

    time_t updatedTime = rtc.time(NULL);
    lastSyncUnixTime = (uint32_t)updatedTime;
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