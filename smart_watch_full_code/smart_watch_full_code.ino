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
#include "ui.h" 

// Custom Sensor & Configuration Libraries
#include "config.h" 
#include "ota.h"
#include "api_sync.h" // <--- NEW API MODULE
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

bool is_charging = false;

// Screen Clear Tracking (60s Rule)
lv_obj_t* current_screen = NULL;
unsigned long left_hr_screen_time = 0;
unsigned long left_temp_screen_time = 0;

// Hardware & Background States
bool is_screen_on = true;
int last_button_val = HIGH;
int button_state = HIGH;
unsigned long last_debounce = 0;

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

// Add under your existing global timers
unsigned long last_activity_time = 0; 
const unsigned long SCREEN_TIMEOUT_MS = 30000; // 30 seconds

// ============================================================
// STORED VALUES
// ============================================================
int stored_hr = 0;
float stored_temp = 0.0;
int stored_hum = 0;
int stored_bp_sys = 0;
int stored_bp_dia = 0;
int stored_bp_hr = 0;
float smoothed_batt_v = 0.0;
int stored_battery = 0;

// Global Toast Pointer
lv_obj_t* toast_label = NULL;

// ============================================================
// UI HELPER FUNCTIONS
// ============================================================
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
    
    last_activity_time = millis(); // <--- ADD THIS: Reset timer on touch
  } else {
    data->state = LV_INDEV_STATE_RELEASED; 
  }
}

void show_toast(const char* msg) {
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
    lv_timer_handler(); 
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
          lv_timer_handler(); 
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
        lv_timer_handler(); 
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
  
  lv_timer_handler();
  http.end();
  
  lv_timer_create([](lv_timer_t* t){ _ui_opacity_set(ui_Popup1, 0); lv_timer_del(t); }, 3000, NULL);
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
    if (stored_temp > 0.0) {
      char temp_buf[10];
      dtostrf(stored_temp, 4, 1, temp_buf);
      lv_label_set_text_fmt(ui_lastBodyTempValue, "%s°C", temp_buf);
      lv_label_set_text_fmt(ui_lastHumidityValue, "%d%%", stored_hum);
    }
    if (stored_battery > 0) {
      lv_label_set_text_fmt(ui_batteryPercentage, "%d", stored_battery);
      lv_arc_set_value(ui_batteryArc, stored_battery);
    }
  }
}

// ============================================================
// MAIN SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);

  init_RTC();
  init_AHT10();
  init_BATTERY();
  init_HEART();
  stop_HEART(); 

  tft.begin();
  tft.setRotation(0); 
  pinMode(TFT_BL_PIN, OUTPUT);
  analogWrite(TFT_BL_PIN, DEFAULT_BRIGHTNESS); 
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
  last_activity_time = millis();
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  lv_timer_handler(); 
  lv_tick_inc(5); 

  // --- HARDWARE BUTTON LOGIC ---
  int reading = digitalRead(BUTTON_PIN); 
  if (reading != last_button_val) last_debounce = millis();
  
  if ((millis() - last_debounce) > 50) {
    if (reading != button_state) {
      button_state = reading;
      if (button_state == LOW) { 
        is_screen_on = !is_screen_on;
        last_activity_time = millis(); // <--- ADD THIS: Reset timer on button press
        
        if (is_screen_on) {
          int val = (int)lv_slider_get_value(ui_Slider1);
          analogWrite(TFT_BL_PIN, map(val, 0, 100, 0, 255));
        } else {
          analogWrite(TFT_BL_PIN, 0);
        }
      }
    }
  }
  last_button_val = reading;

  // --- AUTO SCREEN TIMEOUT LOGIC ---
  if (is_screen_on && (millis() - last_activity_time > SCREEN_TIMEOUT_MS)) {
    is_screen_on = false;
    analogWrite(TFT_BL_PIN, 0); // Turn off backlight
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
      performCloudSync(stored_hr, stored_temp, stored_hum, stored_bp_sys, stored_bp_dia);

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

  if (!is_screen_on) {
    delay(50);
    return; 
  }

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
      
      if (clim.temperature > -10.0 && clim.temperature < 80.0) { 
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
        }
      }
      last_temp_poll = millis();
    }
    if (millis() - temp_meas_start > 20000) { 
      temp_state = SENSOR_ERROR;
      set_button_state(ui_buttonTemp, ui_StartMeasure5, false, 0xB25100, 0);
      lv_label_set_text(ui_tempLive, "--");
      lv_label_set_text(ui_humLive, "--");
      show_toast("Temp Timeout");
    }
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
        lv_timer_handler(); 
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
  }

  // --- PERIODIC TASKS ---
  if (millis() - last_1sec_timer >= 1000) {
    update_clock_ui();
    last_1sec_timer = millis();
  }

  if (millis() - last_battery_timer >= 60000) {
    update_battery();
    last_battery_timer = millis();
  }

  delay(5); 
}