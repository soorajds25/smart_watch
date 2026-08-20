#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==========================================
// 1. HARDWARE PINS
// ==========================================

// I2C Bus (Shared across Touch, AHT10, RTC, MAX30100)
#define I2C_SDA_PIN      11
#define I2C_SCL_PIN      12

// Touch Controller (CST816S)
#define TOUCH_INT_PIN    13
#define TOUCH_RST_PIN    8

// User Interaction & Power
#define BUTTON_PIN       1
#define BATTERY_ADC_PIN  3  // Placeholder: Update to your actual BMS voltage divider pin

// Display Pins (GC9A01)
// Note: These are actively managed by User_Setup.h for TFT_eSPI, 
// File Path : "C:\Users\Username\Documents\Arduino\libraries\TFT_eSPI\User_Setup.h"
// but documented here for project reference and backlight control.
#define TFT_BL_PIN       2
#define TFT_CS_PIN       5
#define TFT_DC_PIN       4
#define TFT_MOSI_PIN     7
#define TFT_SCLK_PIN     9
#define TFT_RST_PIN      6

// ==========================================
// 2. I2C DEVICE ADDRESSES
// ==========================================
#define TOUCH_I2C_ADDR   0x15  // CST816S Touch Interface
#define AHT10_I2C_ADDR   0x38  // AHT10 Temp/Humidity Sensor
#define EEPROM_I2C_ADDR  0x50  // DS1307 Attached EEPROM
#define MAX30100_I2C_ADDR 0x57 // SpO2 & Heart Rate Sensor
#define RTC_I2C_ADDR     0x68  // DS1307 RTC

// ==========================================
// 3. NETWORK & CLOUD CONFIGURATION
// ==========================================
// Use extern for credentials so they can be defined securely in a .cpp file 
// or loaded dynamically via Preferences (NVS) later.
extern const char* WIFI_SSID = "PHENIX-AX10";
extern const char* WIFI_PASS = "03323359065";

#define VERCEL_API_URL   "https://patient-health-monitoring-pi.vercel.app/api/entry"
#define VERCEL_API_KEY   "your_auth_token_here"
#define OTA_UPDATE_URL   "https://your-custom-dashboard.vercel.app/api/firmware.bin"

// ==========================================
// 4. POWER MANAGEMENT & THRESHOLDS
// ==========================================
#define SCREEN_TIMEOUT_MS       10000  // Go to sleep after 10 seconds of inactivity
#define LONG_PRESS_MS           2000   // 2 seconds for a long button press (e.g., power off)
#define SYNC_INTERVAL_MINUTES   60     // Wake Wi-Fi and sync to Vercel every 60 mins

// Battery Calibration (Adjust based on your voltage divider resistors)
#define BATT_MAX_VOLTAGE        4.2f
#define BATT_MIN_VOLTAGE        3.2f

// Display Settings
#define DEFAULT_BRIGHTNESS      128    // PWM value 0-255

#endif // CONFIG_H