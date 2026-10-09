// ============================================================
// INCLUDES
// ============================================================
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Wire.h>
#include <CST816S.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "ui.h" 

// Power management (ESP-IDF APIs, available in the Arduino-ESP32 core)
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

// Custom Sensor & Configuration Libraries
#include "config.h" 
#include "ota.h"
#include "api_sync.h"
#include "rtc.h"
#include "temperature.h"
#include "heart_rate.h"
#include "battery.h"
#include "BP.h"

// ============================================================
// CONFIGURATION
// ============================================================
TFT_eSPI tft = TFT_eSPI();
CST816S touch(I2C_SDA_PIN, I2C_SCL_PIN, TOUCH_RST_PIN, TOUCH_INT_PIN);

// ============================================================
// GLOBAL STATE & TIMERS
// ============================================================
enum SensorState { IDLE, MEASURING, COMPLETE, SENSOR_ERROR };

SensorState hr_state = IDLE;
SensorState temp_state = IDLE;
SensorState bp_state = IDLE;

// Smart Stabilization Tracking
unsigned long hr_meas_start = 0;
int hr_history[5];
int hr_hist_idx = 0;
int hr_hist_count = 0;

unsigned long temp_meas_start = 0;
unsigned long last_temp_poll = 0;
float last_temp_val = 0.0;
int temp_stable_count = 0;

unsigned long bp_last_activity = 0;

// ------------------------------------------------------------
// POWER STATES
//   ACTIVE : screen on, 80 MHz, normal UI
//   IDLE   : backlight off + display in sleep-in, no UI work,
//            CPU light-sleeps between events (button / next task)
//   DEEP   : esp_deep_sleep_start(), wakes ONLY on the button (reboot)
// ------------------------------------------------------------
enum PowerState { PWR_ACTIVE, PWR_IDLE };
PowerState power_state = PWR_ACTIVE;

bool is_charging = false;
bool is_screen_on = true;               // true only in PWR_ACTIVE
bool display_asleep = false;
unsigned long last_activity_time = 0; 
unsigned long idle_start_time = 0;
uint32_t last_lv_tick = 0;

// Hardware Button State
int last_button_val = HIGH;
int button_state = HIGH;
unsigned long last_debounce = 0;

// Screen Clear Tracking (60s Rule)
lv_obj_t* current_screen = NULL;
unsigned long left_hr_screen_time = 0;
unsigned long left_temp_screen_time = 0;

// Network State
bool is_wifi_on = false;
bool wifi_connecting = false;
unsigned long wifi_start_time = 0;

// Sync State Machine (Handles NTP Time + Cloud Data Push)
enum SyncState { SYNC_IDLE, SYNC_START, SYNC_WAIT_WIFI, SYNC_EXECUTE };
SyncState sync_state = SYNC_IDLE;
unsigned long sync_timer = 0;

// OTA Trigger
bool trigger_ota_process = false;

// Non-blocking Timers
unsigned long last_1sec_timer = 0;
unsigned long last_battery_timer = 0;

// Background Auto-Task Timers (intervals live in config.h)
unsigned long last_auto_measure = 0;
unsigned long last_auto_sync = 0;

// ============================================================
// STORED VALUES & HISTORICAL QUEUE
// (RTC_DATA_ATTR = survives deep sleep, lost on power loss)
// ============================================================
RTC_DATA_ATTR int stored_hr = 0;
RTC_DATA_ATTR float stored_temp = 0.0;
RTC_DATA_ATTR int stored_hum = 0;
RTC_DATA_ATTR int stored_spo2 = 0;
RTC_DATA_ATTR int stored_bp_sys = 0;
RTC_DATA_ATTR int stored_bp_dia = 0;
RTC_DATA_ATTR int stored_bp_hr = 0;
float smoothed_batt_v = 0.0;
int stored_battery = 0;

// sent_mask bits: 1 = HR, 2 = temp, 4 = humidity, 8 = SpO2 (already uploaded)
struct HealthRecord {
  int hr;
  int spo2;
  float temp;
  int hum;
  char timestamp[25]; // Holds "YYYY-MM-DDThh:mm:ssZ"
  bool has_data;
  uint8_t sent_mask;
};

const int MAX_RECORDS = 24; // kept after failed syncs (24 x ~44 B in RTC memory)
RTC_DATA_ATTR HealthRecord sync_queue[MAX_RECORDS];
RTC_DATA_ATTR int queue_index = 0;

lv_obj_t* toast_label = NULL;

// ============================================================
// LVGL SERVICE (real elapsed time instead of a fixed 5 ms tick)
// ============================================================
void lvgl_service() {
  uint32_t now = millis();
  lv_tick_inc(now - last_lv_tick);
  last_lv_tick = now;
  lv_timer_handler();
}

// ============================================================
// UI & TIME HELPER FUNCTIONS
// ============================================================
void get_iso_timestamp(char* buffer) {
  RTC_Data t = readRTC(); 
  sprintf(buffer, "%04d-%02d-%02dT%02d:%02d:%02dZ", 
          t.year, t.month, t.day, t.hour, t.minute, t.second);
}

void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp_drv);
}

void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
  if (!is_screen_on) {
    data->state = LV_INDEV_STATE_RELEASED; 
    return; 
  }
  if (touch.available()) {
    data->state = LV_INDEV_STATE_PRESSED; 
    data->point.x = touch.data.x;
    data->point.y = touch.data.y;
    last_activity_time = millis(); // Reset timeout on touch
  } else {
    data->state = LV_INDEV_STATE_RELEASED; 
  }
}

void show_toast(const char* msg) {
  if (power_state == PWR_IDLE) return;   // nobody is looking, don't touch the UI
  if (toast_label != NULL) {
    lv_obj_del(toast_label);
    toast_label = NULL;
  }
  toast_label = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_color(toast_label, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(toast_label, 200, 0);
  lv_obj_set_style_text_color(toast_label, lv_color_white(), 0);
  lv_obj_set_style_pad_all(toast_label, 10, 0);
  lv_obj_set_style_radius(toast_label, 10, 0);
  lv_label_set_text(toast_label, msg);
  lv_obj_align(toast_label, LV_ALIGN_TOP_MID, 0, 20);
  
  lv_timer_create([](lv_timer_t* timer){
    if (toast_label) {
      lv_obj_del(toast_label);
      toast_label = NULL;
    }
    lv_timer_del(timer);
  }, 3000, NULL);
}

void set_button_state(lv_obj_t* btn_obj, lv_obj_t* label_obj, bool is_measuring, uint32_t idle_color, uint32_t active_color) {
  if (is_measuring) {
    lv_label_set_text(label_obj, "Measuring...");
    lv_obj_set_style_bg_color(btn_obj, lv_color_hex(active_color), 0);
  } else {
    lv_label_set_text(label_obj, "Start\nMeasuring");
    lv_obj_set_style_bg_color(btn_obj, lv_color_hex(idle_color), 0);
  }
}

void update_status_icons() {
  if (lv_scr_act() != ui_Screen1) return;

  // ICON 1: Heart Rate / SpO2 Active
  if (hr_state == MEASURING) lv_obj_clear_flag(ui_heartRateIndicator, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(ui_heartRateIndicator, LV_OBJ_FLAG_HIDDEN);

  // ICON 2: Temperature & Humidity Active
  if (temp_state == MEASURING) lv_obj_clear_flag(ui_tempAndHumidityIndicator, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(ui_tempAndHumidityIndicator, LV_OBJ_FLAG_HIDDEN);

  // ICON 3: Wi-Fi Sync Active
  if (sync_state != SYNC_IDLE) lv_obj_clear_flag(ui_wifiIndicator, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(ui_wifiIndicator, LV_OBJ_FLAG_HIDDEN);

  // ICON 4: BP Active 
  if (bp_state == MEASURING) lv_obj_clear_flag(ui_BPMonitorIndicator, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(ui_BPMonitorIndicator, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
// EVENT CALLBACKS 
// ============================================================
void start_hr_measurement(lv_event_t * e) {
  if (hr_state != MEASURING) {
    hr_state = MEASURING;
    hr_meas_start = millis();
    hr_hist_idx = 0;
    hr_hist_count = 0;
    start_HEART(); 
    set_button_state(ui_buttonHeart, ui_StartMeasure2, true, 0xFA4040, 0x880000);
    lv_label_set_text(ui_heartRateLive, "--"); 
  }
}

void start_temp_measurement(lv_event_t * e) {
  if (temp_state != MEASURING) {
    temp_state = MEASURING;
    temp_meas_start = millis();
    last_temp_poll = 0;
    temp_stable_count = 0;
    last_temp_val = 0.0;
    set_button_state(ui_buttonTemp, ui_StartMeasure5, true, 0xB25100, 0x602000);
    lv_label_set_text(ui_tempLive, "--");
    lv_label_set_text(ui_humLive, "--");
  }
}

void start_bp_measurement(lv_event_t * e) {
  if (bp_state != MEASURING) {
    bp_state = MEASURING;
    bp_last_activity = millis();
    init_BP(); 
    set_button_state(ui_buttonBP, ui_StartMeasure4, true, 0x007E79, 0x004040);
    lv_label_set_text(ui_sysLive, "--");
    lv_label_set_text(ui_diaLive, "--");
    lv_label_set_text(ui_heartRateLiveBP, "-- BPM");
  }
}

void start_sync_event(lv_event_t * e) {
  if (sync_state == SYNC_IDLE) {
    Serial.println("\nDEBUG: [SYNC] 'Sync Now' Button Pressed!");
    sync_state = SYNC_START;
    show_toast("Starting Sync...");
  }
}

void wifi_toggle(lv_event_t * e) {
  lv_obj_t * sw = lv_event_get_target(e);
  is_wifi_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (is_wifi_on) {
    show_toast("Wi-Fi Connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS); 
    wifi_connecting = true;
    wifi_start_time = millis();
  } else {
    show_toast("Wi-Fi OFF");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    wifi_connecting = false;
  }
}

void brightness_slider_event_cb(lv_event_t * e) {
  lv_obj_t * slider = lv_event_get_target(e);
  int val = (int)lv_slider_get_value(slider);
  analogWrite(TFT_BL_PIN, map(val, 0, 100, 0, 255));
}

void screen8_loaded_cb(lv_event_t * e) {
  lv_label_set_text(ui_currentVersionNumber, currentFirmwareVersion);
  if (is_wifi_on && WiFi.status() == WL_CONNECTED) {
    lv_label_set_text(ui_latestVersionNumber, "Checking...");
    lvgl_service(); 
    HTTPClient http;
    http.begin(versionUrl);
    if (http.GET() == HTTP_CODE_OK) {
      String latest = http.getString();
      latest.trim();
      lv_label_set_text(ui_latestVersionNumber, latest.c_str());
    } else {
      lv_label_set_text(ui_latestVersionNumber, "Error");
    }
    http.end();
  } else {
    lv_label_set_text(ui_latestVersionNumber, "No WiFi");
  }
}

void screen8_unloaded_cb(lv_event_t * e) {
  _ui_opacity_set(ui_Popup1, 0);
  _ui_opacity_set(ui_Popup2, 0);
}

void ota_confirm_event_cb(lv_event_t * e) {
  trigger_ota_process = true; 
}

// ============================================================
// OTA CORE LOGIC
// ============================================================
String fetchLatestVersion() {
  HTTPClient http;
  http.begin(versionUrl);
  if (http.GET() == HTTP_CODE_OK) {
    String latest = http.getString();
    latest.trim();
    http.end();
    return latest;
  }
  http.end();
  return "";
}

bool startOTAUpdate(WiFiClient* client, int contentLength) {
  if (!Update.begin(contentLength)) return false;

  size_t written = 0;
  int progress = 0;
  int lastProgress = 0;
  unsigned long lastDataTime = millis();

  while (written < contentLength) {
    if (client->available()) {
      uint8_t buffer[128];
      size_t len = client->read(buffer, sizeof(buffer));
      if (len > 0) {
        Update.write(buffer, len);
        written += len;

        progress = (written * 100) / contentLength;
        if (progress != lastProgress) {
          lv_slider_set_value(ui_upgradeProgressBarPopUp, progress, LV_ANIM_OFF);
          char buf[32];
          sprintf(buf, "UPGRADING: %d%%", progress);
          lv_label_set_text(ui_UpgradingTextPopUp, buf);
          lvgl_service(); 
          lastProgress = progress;
        }
      }
      lastDataTime = millis();
    }
    
    if (millis() - lastDataTime > 15000) { 
      Update.abort();
      return false;
    }
    yield();
  }

  return (written == contentLength) ? Update.end() : false;
}

void downloadAndApplyFirmware(String targetVersion) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String dynamicUrl = firmwareBaseUrl + targetVersion + firmwareFilename;
  
  http.begin(dynamicUrl);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    if (contentLength > 0) {
      WiFiClient* stream = http.getStreamPtr();
      if (startOTAUpdate(stream, contentLength)) {
        lv_label_set_text(ui_UpgradingTextPopUp, "SUCCESS! RESTARTING...");
        lvgl_service(); 
        delay(2000);
        ESP.restart();
      } else {
        lv_label_set_text(ui_UpgradingTextPopUp, "UPDATE FAILED!");
      }
    } else {
      lv_label_set_text(ui_UpgradingTextPopUp, "INVALID SIZE!");
    }
  } else {
    lv_label_set_text(ui_UpgradingTextPopUp, "DOWNLOAD FAILED!");
  }
  
  lvgl_service();
  http.end();
  
  lv_timer_create([](lv_timer_t* t){ _ui_opacity_set(ui_Popup1, 0); lv_timer_del(t); }, 3000, NULL);
}

// ============================================================
// CLOUD SYNC LOGIC
// ============================================================
// Uploads one reading. Returns true only if the cloud accepted it.
static bool upload_one(const char* key, float value, bool as_int, const char* ts) {
  StaticJsonDocument<128> doc;
  if (as_int) doc[key] = (int)value;
  else        doc[key] = value;
  doc["recorded_at"] = ts;
  return sendReadingToCloud(doc);
}

// Per-field "already uploaded" tracking: a failed upload is retried on the next
// sync, and fields that DID upload are never sent twice.
bool performHistoricalCloudSync() {
  Serial.println("\nDEBUG: [API] --- UPLOADING HISTORICAL QUEUE ---");
  bool overallSuccess = true;

  // 1. Upload Manual BP Reading if exists
  if (stored_bp_sys > 0 && stored_bp_dia > 0) {
    Serial.println("DEBUG: [API] Uploading BP...");
    StaticJsonDocument<128> sysDoc, diaDoc;
    sysDoc["bp_value"] = stored_bp_sys; sysDoc["bp_type"] = "systolic";
    diaDoc["bp_value"] = stored_bp_dia; diaDoc["bp_type"] = "diastolic";
    
    if (!sendReadingToCloud(sysDoc) || !sendReadingToCloud(diaDoc)) overallSuccess = false;
    else {
      stored_bp_sys = 0; // Clear after successful sync
      stored_bp_dia = 0;
    }
  }

  // 2. Upload Historical Array
  for (int i = 0; i < queue_index; i++) {
    HealthRecord &r = sync_queue[i];
    if (!r.has_data) continue;
    Serial.printf("Uploading Record %d/%d taken at %s\n", i+1, queue_index, r.timestamp);

    uint8_t needed = 0;
    if (r.hr > 0)      needed |= 1;
    if (r.temp > 0.0)  needed |= 2;
    if (r.hum > 0)     needed |= 4;
    if (r.spo2 > 0)    needed |= 8;

    if ((needed & 1) && !(r.sent_mask & 1)) {
      if (upload_one("heart_rate", r.hr, true, r.timestamp)) r.sent_mask |= 1; else overallSuccess = false;
    }
    if ((needed & 2) && !(r.sent_mask & 2)) {
      if (upload_one("temperature", r.temp, false, r.timestamp)) r.sent_mask |= 2; else overallSuccess = false;
    }
    if ((needed & 4) && !(r.sent_mask & 4)) {
      if (upload_one("humidity", r.hum, true, r.timestamp)) r.sent_mask |= 4; else overallSuccess = false;
    }
    if ((needed & 8) && !(r.sent_mask & 8)) {
      if (upload_one("spo2", r.spo2, true, r.timestamp)) r.sent_mask |= 8; else overallSuccess = false;
    }

    if ((r.sent_mask & needed) == needed) r.has_data = false;  // fully uploaded
  }

  // 3. Compact: drop fully-uploaded records, keep the failed ones for next time
  int w = 0;
  for (int r = 0; r < queue_index; r++) {
    if (sync_queue[r].has_data) {
      if (w != r) sync_queue[w] = sync_queue[r];
      w++;
    }
  }
  queue_index = w;

  if (overallSuccess) Serial.println("DEBUG: [API] Queue cleared successfully.");
  else Serial.printf("DEBUG: [API] %d record(s) kept for retry.\n", queue_index);
  return overallSuccess;
}

// ============================================================
// BATTERY & CLOCK LOGIC
// ============================================================
void update_battery() {
  float total_v = 0;
  for(int i = 0; i < 10; i++) {
    uint32_t adc_mv = analogReadMilliVolts(BATTERY_ADC_PIN);
    total_v += (adc_mv / 1000.0);
  }
  float raw_batt_v = (total_v / 10.0) * VOLTAGE_DIVIDER_RATIO;

  // Charging Detection (Adjust 4.22 based on your BMS)
  if (raw_batt_v >= 4.22) is_charging = true;
  else is_charging = false;

  if (smoothed_batt_v == 0.0) smoothed_batt_v = raw_batt_v; 
  smoothed_batt_v = (smoothed_batt_v * 0.8) + (raw_batt_v * 0.2);

  stored_battery = calculateBatteryPercentage(smoothed_batt_v);
}

void update_clock_ui() {
  RTC_Data t = readRTC();
  
  if (lv_scr_act() == ui_Screen1) {
    lv_label_set_text_fmt(ui_time, "%02d:%02d", t.hour, t.minute);
    lv_label_set_text_fmt(ui_date, "%02d", t.day);
    
    if (stored_hr > 0) {
      lv_label_set_text_fmt(ui_lastHeartRate, "%d", stored_hr);
      lv_arc_set_value(ui_heartRateArc, stored_hr);
    }
    // Check if it's not exactly 0.0 (allowing for negative winter temps)
    if (stored_temp != 0.0 || stored_hum != 0) {
      char temp_buf[10];
      dtostrf(stored_temp, 4, 1, temp_buf);
      lv_label_set_text_fmt(ui_lastBodyTempValue, "%s°C", temp_buf);
      lv_label_set_text_fmt(ui_lastHumidityValue, "%d%%", stored_hum);
    }
    if (stored_battery > 0) {
      if (is_charging) {
        lv_label_set_text_fmt(ui_batteryPercentage, LV_SYMBOL_CHARGE " %d", stored_battery);
        lv_obj_set_style_text_color(ui_batteryPercentage, lv_color_hex(0x00FF00), 0); 
      } else {
        lv_label_set_text_fmt(ui_batteryPercentage, "%d", stored_battery);
        lv_obj_set_style_text_color(ui_batteryPercentage, lv_color_hex(0xFFFFFF), 0); 
      }
      lv_arc_set_value(ui_batteryArc, stored_battery);
    }
  }
}

// ============================================================
// POWER MANAGEMENT
// ============================================================

// True while something must NOT be interrupted by sleeping.
bool sleep_inhibited() {
  return hr_state == MEASURING || temp_state == MEASURING || bp_state == MEASURING ||
         sync_state != SYNC_IDLE || trigger_ota_process || wifi_connecting;
}

// GC9A01 sleep-in / sleep-out (generic MIPI commands)
void display_sleep() {
  if (display_asleep) return;
  tft.writecommand(0x28);   // DISPOFF
  delay(10);
  tft.writecommand(0x10);   // SLPIN
  delay(10);
  display_asleep = true;
}

void display_wake() {
  if (!display_asleep) return;
  tft.writecommand(0x11);   // SLPOUT
  delay(120);               // required settle time after SLPOUT
  tft.writecommand(0x29);   // DISPON
  delay(20);
  display_asleep = false;
}

// Wi-Fi is never kept on while the screen is off (unless a sync is running;
// that sync turns it off itself when done).
void wifi_force_off_for_idle() {
  if (is_wifi_on && sync_state == SYNC_IDLE) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    is_wifi_on = false;
    wifi_connecting = false;
    lv_obj_clear_state(ui_wifiSwitch, LV_STATE_CHECKED);  // no event fired by this
  }
}

void enter_idle() {
  if (power_state == PWR_IDLE) return;
  if (toast_label != NULL) { lv_obj_del(toast_label); toast_label = NULL; }
  wifi_force_off_for_idle();

  analogWrite(TFT_BL_PIN, 0);
  display_sleep();
  power_state = PWR_IDLE;
  is_screen_on = false;
  idle_start_time = millis();
  Serial.println("DEBUG: [PWR] -> IDLE");
}

void wake_to_active() {
  if (power_state == PWR_ACTIVE) return;
  display_wake();
  int val = (int)lv_slider_get_value(ui_Slider1);
  analogWrite(TFT_BL_PIN, map(val, 0, 100, 0, 255));
  power_state = PWR_ACTIVE;
  is_screen_on = true;
  last_activity_time = millis();
  last_lv_tick = millis();
  lv_obj_invalidate(lv_scr_act());
  update_clock_ui();
  update_status_icons();
  Serial.println("DEBUG: [PWR] -> ACTIVE");
}

// Optional: one last upload attempt so queued data isn't left waiting for the next wake.
void final_sync_before_deep_sleep() {
  if (queue_index == 0 && stored_bp_sys == 0) return;
  Serial.println("DEBUG: [PWR] Final sync before deep sleep...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeRTC();
    performHistoricalCloudSync();
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

void enter_deep_sleep() {
  Serial.println("DEBUG: [PWR] -> DEEP SLEEP");

  // 1. Stop sensors / measurements
  stop_HEART();
  if (bp_state == MEASURING) stop_BP();
  hr_state = IDLE; temp_state = IDLE; bp_state = IDLE;

  // 2. Wi-Fi: optional final upload, then radio off
#if DEEP_SLEEP_FINAL_SYNC
  final_sync_before_deep_sleep();
#endif
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  // 3. Display: backlight off, controller in sleep-in
  analogWrite(TFT_BL_PIN, 0);
  display_sleep();

  // 4. Keep display lines in a safe state while the MCU is asleep
  digitalWrite(TFT_CS_PIN, HIGH);
  pinMode(TFT_RST_PIN, OUTPUT);
  digitalWrite(TFT_RST_PIN, HIGH);
  gpio_hold_en((gpio_num_t)TFT_BL_PIN);   // backlight stays LOW
  gpio_hold_en((gpio_num_t)TFT_CS_PIN);
  gpio_hold_en((gpio_num_t)TFT_RST_PIN);
  gpio_deep_sleep_hold_en();

  // 5. Wake source: button on GPIO 1 (active LOW, RTC GPIO on ESP32-S3)
  unsigned long t = millis();
  while (digitalRead(BUTTON_PIN) == LOW && millis() - t < 3000) delay(10);  // wait for release
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);

  Serial.flush();
  esp_deep_sleep_start();
}

// Light sleep until the button is pressed or `ms` elapsed. millis() keeps counting.
void light_sleep_ms(uint32_t ms) {
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  Serial.flush();
  esp_light_sleep_start();
  gpio_wakeup_disable((gpio_num_t)BUTTON_PIN);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
}

static uint32_t remaining_ms(unsigned long start, unsigned long period) {
  unsigned long el = millis() - start;
  return (el >= period) ? 0 : (uint32_t)(period - el);
}

// Called once per loop() while in IDLE, after all background work has run.
void idle_power_step() {
  wifi_force_off_for_idle();

  // Don't sleep during work in progress or while a button press is being debounced
  if (sleep_inhibited() || button_state == LOW || digitalRead(BUTTON_PIN) != button_state) {
    delay(5);
    return;
  }

#if DEEP_SLEEP_ENABLED
  if (millis() - idle_start_time >= DEEP_SLEEP_TIMEOUT_MS) {
    enter_deep_sleep();
    return;
  }
  uint32_t next = remaining_ms(idle_start_time, DEEP_SLEEP_TIMEOUT_MS);
#else
  uint32_t next = 0xFFFFFFFFUL;
#endif

  uint32_t m = remaining_ms(last_auto_measure, AUTO_MEASURE_INTERVAL_MINUTES * 60000UL);
  uint32_t s = remaining_ms(last_auto_sync,    AUTO_SYNC_INTERVAL_MINUTES * 60000UL);
  if (m < next) next = m;
  if (s < next) next = s;

  if (next < 20) { delay(5); return; }
  light_sleep_ms(next + 5);   // +5 ms so the task is really due when we wake
}

// ============================================================
// MAIN SETUP
// ============================================================
void setup() {
  // Release pin holds left from deep sleep
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)TFT_BL_PIN);
  gpio_hold_dis((gpio_num_t)TFT_CS_PIN);
  gpio_hold_dis((gpio_num_t)TFT_RST_PIN);

  setCpuFrequencyMhz(ACTIVE_CPU_MHZ);
  Serial.begin(115200);

  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  if (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
    rtc_gpio_deinit((gpio_num_t)BUTTON_PIN);   // give GPIO 1 back to the digital domain
    Serial.println("DEBUG: [PWR] Woke from deep sleep (button)");
  }
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Start from the real button level so the press that woke us isn't seen as a new press
  last_button_val = digitalRead(BUTTON_PIN);
  button_state = last_button_val;

  // Keep the backlight off until the first frame is drawn (no flash on boot/wake)
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, LOW);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);

  init_RTC();
  init_AHT10();
  init_BATTERY();
  init_HEART();
  stop_HEART(); 

  tft.begin();
  tft.setRotation(0); 
  touch.begin();
  
  lv_init();
  static lv_disp_draw_buf_t draw_buf;
  static lv_color_t buf[240 * 60]; 
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, 240 * 60);
  
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 240;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
  
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read; 
  lv_indev_drv_register(&indev_drv);
  
  ui_init();
  current_screen = ui_Screen1;

  // Enforce Clean Defaults on Boot
  lv_label_set_text(ui_heartRateLive, "--");
  lv_label_set_text(ui_sysLive, "--");
  lv_label_set_text(ui_diaLive, "--");
  lv_label_set_text(ui_heartRateLiveBP, "-- BPM");
  lv_label_set_text(ui_tempLive, "--");
  lv_label_set_text(ui_humLive, "--");
  lv_label_set_text(ui_SpO2Live, "--");
  lv_label_set_text(ui_lastSpO2Value, "--");
  lv_arc_set_value(ui_spO2Arc, 0);

  // Bind UI Callbacks
  lv_obj_add_event_cb(ui_buttonHeart, start_hr_measurement, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(ui_buttonTemp, start_temp_measurement, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(ui_buttonBP, start_bp_measurement, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(ui_wifiSwitch, wifi_toggle, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(ui_Slider1, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(ui_ButtonSyncNow, start_sync_event, LV_EVENT_CLICKED, NULL);
  
  lv_obj_add_event_cb(ui_Screen8, screen8_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
  lv_obj_add_event_cb(ui_Screen8, screen8_unloaded_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
  lv_obj_add_event_cb(ui_confirmButton, ota_confirm_event_cb, LV_EVENT_RELEASED, NULL);

  update_battery();
  update_clock_ui();            // shows restored values (HR/temp/etc.) after deep sleep

  // Draw the first frame, then switch the backlight on
  last_lv_tick = millis();
  delay(40);
  lvgl_service();
  delay(40);
  lvgl_service();
  analogWrite(TFT_BL_PIN, DEFAULT_BRIGHTNESS); 

  last_activity_time = millis();
  last_auto_measure = millis();
  last_auto_sync = millis();
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  if (power_state == PWR_ACTIVE) lvgl_service();   // no UI work while the screen is off

  // --- HARDWARE BUTTON LOGIC ---
  int reading = digitalRead(BUTTON_PIN); 
  if (reading != last_button_val) last_debounce = millis();
  
  if ((millis() - last_debounce) > 50) {
    if (reading != button_state) {
      button_state = reading;
      if (button_state == LOW) { 
        last_activity_time = millis(); // Reset timer on button press
        if (power_state == PWR_ACTIVE) enter_idle();
        else wake_to_active();
      }
    }
  }
  last_button_val = reading;

  // --- ACTIVE -> IDLE AFTER INACTIVITY ---
  if (power_state == PWR_ACTIVE && !sleep_inhibited() &&
      (millis() - last_activity_time > IDLE_TIMEOUT_MS)) {
    enter_idle();
  }

  // --- WI-FI CONNECTION TIMEOUT TOAST ---
  if (is_wifi_on && wifi_connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      show_toast("Wi-Fi Connected!");
      wifi_connecting = false;
    } else if (millis() - wifi_start_time > 8000) {
      show_toast("Cannot Connect Wi-Fi");
      wifi_connecting = false; 
    }
  }

  // --- NON-BLOCKING SYNC & CLOUD UPLOAD LOGIC ---
  if (sync_state == SYNC_START) {
    Serial.println("DEBUG: [SYNC] Activating Wi-Fi for Sync...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    sync_timer = millis();
    sync_state = SYNC_WAIT_WIFI;
  } 
  else if (sync_state == SYNC_WAIT_WIFI) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("DEBUG: [SYNC] Wi-Fi Connected! Syncing Time & Data...");
      show_toast("Syncing Time & Data...");
      
      syncTimeRTC();
      performHistoricalCloudSync(); // Uploads the queue (failed records are kept)

      sync_state = SYNC_EXECUTE;
      sync_timer = millis();
    } else if (millis() - sync_timer > 10000) {
      Serial.println("DEBUG: [SYNC] Failed - Wi-Fi Timeout!");
      show_toast("Sync Failed: No Wi-Fi");
      if (!is_wifi_on) { WiFi.disconnect(true, true); WiFi.mode(WIFI_OFF); }
      sync_state = SYNC_IDLE;
    }
  } 
  else if (sync_state == SYNC_EXECUTE) {
    Serial.println("DEBUG: [SYNC] Task Complete. Restoring previous Wi-Fi state.");
    show_toast("Sync Complete!");
    if (!is_wifi_on) {
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
    }
    sync_state = SYNC_IDLE;
  }

  // --- SCREEN CANCELLATION TRAP & TIMER ARMING ---
  lv_obj_t* act_scr = lv_scr_act();
  if (current_screen != act_scr) {
    if (current_screen == ui_Screen2 && hr_state == MEASURING) {
      stop_HEART();
      hr_state = IDLE;
      set_button_state(ui_buttonHeart, ui_StartMeasure2, false, 0xFA4040, 0);
      lv_label_set_text(ui_heartRateLive, "--");
    }
    else if (current_screen == ui_Screen4 && bp_state == MEASURING) {
      stop_BP();
      bp_state = IDLE;
      set_button_state(ui_buttonBP, ui_StartMeasure4, false, 0x007E79, 0);
      lv_label_set_text(ui_sysLive, "--");
      lv_label_set_text(ui_diaLive, "--");
      lv_label_set_text(ui_heartRateLiveBP, "-- BPM");
    }
    else if (current_screen == ui_Screen5 && temp_state == MEASURING) {
      temp_state = IDLE;
      set_button_state(ui_buttonTemp, ui_StartMeasure5, false, 0xB25100, 0);
      lv_label_set_text(ui_tempLive, "--");
      lv_label_set_text(ui_humLive, "--");
    }

    if (current_screen == ui_Screen2) left_hr_screen_time = millis();
    if (current_screen == ui_Screen5) left_temp_screen_time = millis();

    if (act_scr == ui_Screen2) left_hr_screen_time = 0;
    if (act_scr == ui_Screen5) left_temp_screen_time = 0;

    current_screen = act_scr;
  }

  if (left_hr_screen_time > 0 && act_scr != ui_Screen2 && (millis() - left_hr_screen_time > 60000)) {
    lv_label_set_text(ui_heartRateLive, "--");
    left_hr_screen_time = 0;
  }
  if (left_temp_screen_time > 0 && act_scr != ui_Screen5 && (millis() - left_temp_screen_time > 60000)) {
    lv_label_set_text(ui_tempLive, "--");
    lv_label_set_text(ui_humLive, "--");
    left_temp_screen_time = 0;
  }

  // NOTE: the old "if (!is_screen_on) return;" is gone on purpose. Sensor state
  // machines, queueing and the 5 min / 60 min background tasks now keep running
  // while the screen is off (they used to freeze).

  // ============================================================
  // SENSOR STATE MACHINES
  // ============================================================
  
  // --- HEART RATE ---
  if (hr_state == MEASURING) {
    int bpm = readHEART();
    
    if (bpm > 40 && bpm < 220) { 
      lv_label_set_text_fmt(ui_heartRateLive, "%d", bpm); 
      hr_history[hr_hist_idx] = bpm;
      hr_hist_idx = (hr_hist_idx + 1) % 5;
      hr_hist_count++;

      if (hr_hist_count >= 5) {
        int min_hr = 999, max_hr = 0, sum = 0;
        for(int i=0; i<5; i++) {
          if (hr_history[i] < min_hr) min_hr = hr_history[i];
          if (hr_history[i] > max_hr) max_hr = hr_history[i];
          sum += hr_history[i];
        }
        if (max_hr - min_hr <= 3) { 
          stored_hr = sum / 5;
          hr_state = COMPLETE;
          stop_HEART();
          set_button_state(ui_buttonHeart, ui_StartMeasure2, false, 0xFA4040, 0);
          lv_label_set_text_fmt(ui_heartRateLive, "%d", stored_hr);
        }
      }
    } 

    if (millis() - hr_meas_start > 20000) { 
      stop_HEART();
      hr_state = SENSOR_ERROR;
      lv_label_set_text(ui_heartRateLive, "--");
      set_button_state(ui_buttonHeart, ui_StartMeasure2, false, 0xFA4040, 0);
      show_toast("HR Timeout");
    }
  }

  // --- TEMPERATURE & HUMIDITY ---
  if (temp_state == MEASURING) {
    if (millis() - last_temp_poll > 2000) {
      AHT10_Data clim = readAHT10();
      
      Serial.printf("DEBUG: [TEMP POLL] %.2f C, %.2f %%\n", clim.temperature, clim.humidity);
      
      // EXCLUDE FAKE 0.0/0.0 READINGS FROM DEAD SENSORS
      bool is_fake_zero = (clim.temperature == 0.0 && clim.humidity == 0.0);

      if (clim.temperature > -10.0 && clim.temperature < 80.0 && !is_fake_zero) { 
        char temp_buf[10];
        dtostrf(clim.temperature, 4, 1, temp_buf);
        lv_label_set_text_fmt(ui_tempLive, "%s°C", temp_buf);
        lv_label_set_text_fmt(ui_humLive, "%d%%", (int)clim.humidity);
        
        float diff = abs(clim.temperature - last_temp_val);
        last_temp_val = clim.temperature;
        
        if (diff <= 0.2) temp_stable_count++;
        else temp_stable_count = 0;

        if (temp_stable_count >= 3) { 
          stored_temp = clim.temperature;
          stored_hum = (int)clim.humidity;
          temp_state = COMPLETE;
          set_button_state(ui_buttonTemp, ui_StartMeasure5, false, 0xB25100, 0);
          
          Serial.println("DEBUG: [TEMP] Measurement COMPLETE!");
          show_toast("Temp Updated!"); // Show user it succeeded
        }
      } else {
        // If it's 0.0, reset the stability counter so it doesn't fake a completion
        temp_stable_count = 0; 
      }
      last_temp_poll = millis();
    }
    
    if (millis() - temp_meas_start > 20000) { 
      temp_state = SENSOR_ERROR;
      set_button_state(ui_buttonTemp, ui_StartMeasure5, false, 0xB25100, 0);
      lv_label_set_text(ui_tempLive, "--");
      lv_label_set_text(ui_humLive, "--");
      Serial.println("DEBUG: [TEMP] TIMEOUT - Sensor failed to return valid data.");
      
      // Wait slightly so it doesn't get immediately overwritten by the HR timeout
      if (hr_state != MEASURING) {
         show_toast("Temp Timeout");
      }
    }
  }

  // --- QUEUE RECORDING TRAP (Saves reading if both sensors finished) ---
  if ((hr_state == COMPLETE || hr_state == SENSOR_ERROR) && 
      (temp_state == COMPLETE || temp_state == SENSOR_ERROR)) {
    
    // Only save if at least one sensor succeeded
    if (hr_state == COMPLETE || temp_state == COMPLETE) {
      if (queue_index >= MAX_RECORDS) {
        // Queue full (several failed syncs): drop the oldest record, keep the newest
        for (int i = 1; i < MAX_RECORDS; i++) sync_queue[i - 1] = sync_queue[i];
        queue_index = MAX_RECORDS - 1;
      }
      sync_queue[queue_index].hr = (hr_state == COMPLETE) ? stored_hr : 0;
      sync_queue[queue_index].temp = (temp_state == COMPLETE) ? stored_temp : 0.0;
      sync_queue[queue_index].hum = (temp_state == COMPLETE) ? stored_hum : 0;
      sync_queue[queue_index].spo2 = stored_spo2; 
      sync_queue[queue_index].sent_mask = 0;
      
      get_iso_timestamp(sync_queue[queue_index].timestamp);
      sync_queue[queue_index].has_data = true;
      queue_index++;
    }
    // Return states to IDLE to wait for the next 5 min interval
    hr_state = IDLE;
    temp_state = IDLE;
  }

  // --- BLOOD PRESSURE ---
  if (bp_state == MEASURING) {
    BP_Data bp = readBP();
    
    if (bp.status == 's' || bp.status == 'w') {
      bp_last_activity = millis(); 
    } 
    else if (bp.status == 'v') { 
      stored_bp_sys = bp.sys;
      stored_bp_dia = bp.dia;
      stored_bp_hr = bp.heart; 
      stop_BP();
      bp_state = COMPLETE;
      set_button_state(ui_buttonBP, ui_StartMeasure4, false, 0x007E79, 0);
      lv_label_set_text_fmt(ui_sysLive, "%d mmHg", stored_bp_sys);
      lv_label_set_text_fmt(ui_diaLive, "%d mmHg", stored_bp_dia);
      lv_label_set_text_fmt(ui_heartRateLiveBP, "%d BPM", stored_bp_hr);
    } 
    else if (bp.status == 'e' || bp.status == 'o') { 
      stop_BP();
      bp_state = SENSOR_ERROR;
      set_button_state(ui_buttonBP, ui_StartMeasure4, false, 0x007E79, 0);
      lv_label_set_text(ui_sysLive, "--");
      lv_label_set_text(ui_diaLive, "--");
      lv_label_set_text(ui_heartRateLiveBP, "-- BPM");
      show_toast("BP Error/Finished");
    }

    if (millis() - bp_last_activity > 10000) { 
      stop_BP();
      bp_state = SENSOR_ERROR;
      set_button_state(ui_buttonBP, ui_StartMeasure4, false, 0x007E79, 0);
      lv_label_set_text(ui_sysLive, "--");
      lv_label_set_text(ui_diaLive, "--");
      lv_label_set_text(ui_heartRateLiveBP, "-- BPM");
      show_toast("BP Disconnected");
    }
  }

  // ============================================================
  // OTA UPGRADE PROCESSOR
  // ============================================================
  if (trigger_ota_process) {
    trigger_ota_process = false;
    
    if (!is_wifi_on || WiFi.status() != WL_CONNECTED) {
      _ui_opacity_set(ui_Popup1, 0); 
      _ui_opacity_set(ui_Popup2, 255); 
      static lv_obj_t* p2_lbl = NULL;
      if(!p2_lbl) { p2_lbl = lv_label_create(ui_Popup2); lv_obj_center(p2_lbl); lv_obj_set_style_text_color(p2_lbl, lv_color_hex(0xFFFFFF), 0); }
      lv_label_set_text(p2_lbl, "Wi-Fi not connected!");
      
      lv_timer_create([](lv_timer_t* t){ _ui_opacity_set(ui_Popup2, 0); lv_timer_del(t); }, 3000, NULL);
    } else {
      String latest = fetchLatestVersion();
      if (latest != "" && latest != currentFirmwareVersion) {
        _ui_opacity_set(ui_Popup1, 255);
        lv_slider_set_value(ui_upgradeProgressBarPopUp, 0, LV_ANIM_OFF);
        lv_label_set_text(ui_UpgradingTextPopUp, "DOWNLOADING...");
        lvgl_service(); 
        downloadAndApplyFirmware(latest);
      } else {
        _ui_opacity_set(ui_Popup1, 0); 
        _ui_opacity_set(ui_Popup2, 255); 
        static lv_obj_t* p2_lbl = NULL;
        if(!p2_lbl) { p2_lbl = lv_label_create(ui_Popup2); lv_obj_center(p2_lbl); lv_obj_set_style_text_color(p2_lbl, lv_color_hex(0xFFFFFF), 0);
        }
        lv_label_set_text(p2_lbl, "System is up to date!");
        
        lv_timer_create([](lv_timer_t* t){ _ui_opacity_set(ui_Popup2, 0); lv_timer_del(t); }, 3000, NULL);
      }
    }
    last_activity_time = millis();   // OTA counts as activity
  }

  // ============================================================
  // PERIODIC TASKS
  // ============================================================
  if (power_state == PWR_ACTIVE && millis() - last_1sec_timer >= 1000) {
    update_clock_ui();
    update_status_icons();
    last_1sec_timer = millis();
  }

  // Battery: every 5 s with the screen on, every 60 s while idle
  unsigned long batt_period = (power_state == PWR_ACTIVE) ? 5000UL : 60000UL;
  if (millis() - last_battery_timer >= batt_period) {
    update_battery();
    last_battery_timer = millis();
  }

  // ============================================================
  // BACKGROUND AUTOMATION (MEASURE & SYNC) - runs in ACTIVE and IDLE
  // ============================================================
  if (millis() - last_auto_measure >= (AUTO_MEASURE_INTERVAL_MINUTES * 60000UL)) {
    last_auto_measure = millis(); 

    if (hr_state == IDLE) {
      hr_state = MEASURING;
      hr_meas_start = millis();
      hr_hist_idx = 0;
      hr_hist_count = 0;
      start_HEART();
    }
    
    if (temp_state == IDLE) {
      temp_state = MEASURING;
      temp_meas_start = millis();
      last_temp_poll = 0;
      temp_stable_count = 0;
      last_temp_val = 0.0;
    }
  }

  if (millis() - last_auto_sync >= (AUTO_SYNC_INTERVAL_MINUTES * 60000UL)) {
    last_auto_sync = millis(); 
    
    if (sync_state == SYNC_IDLE) {
      Serial.println("DEBUG: [AUTO] Initiating Background Sync...");
      sync_state = SYNC_START; 
    }
  }

  // ============================================================
  // SLEEP
  // ============================================================
  if (power_state == PWR_IDLE) {
    idle_power_step();   // light sleep, or deep sleep after DEEP_SLEEP_TIMEOUT_MS
  } else {
    delay(5);
  }
}
