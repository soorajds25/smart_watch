// ============================================================
// INCLUDES
// ============================================================
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Wire.h>
#include <CST816S.h>
#include <WiFi.h>
#include "ui.h" 

// Custom Sensor Libraries
#include "config.h" 
#include "rtc.h"
#include "temperature.h"
#include "heart_rate.h"
#include "battery.h"
#include "BP.h"

// ============================================================
// CONFIGURATION
// ============================================================
TFT_eSPI tft = TFT_eSPI();
CST816S touch(I2C_SDA_PIN, I2C_SCL_PIN, TOUCH_RST_PIN, TOUCH_INT_PIN); // from config.h

// ============================================================
// GLOBAL STATE & TIMERS
// ============================================================
enum SensorState { IDLE, MEASURING, COMPLETE, SENSOR_ERROR };

SensorState hr_state = IDLE;
SensorState temp_state = IDLE;
SensorState bp_state = IDLE;

// Measurement Windows & Tracking
unsigned long hr_meas_start = 0;
int hr_valid_reads = 0;

unsigned long temp_meas_start = 0;
unsigned long last_temp_poll = 0;

unsigned long bp_meas_start = 0;

// Screen tracking for the 60-second timeout & Cancellation
lv_obj_t* current_screen = NULL;
lv_obj_t* timed_out_screen = NULL;
unsigned long screen_left_timer = 0;
bool screen_timeout_active = false;

// Hardware states
bool is_screen_on = true;
int last_button_val = HIGH;
int button_state = HIGH;
unsigned long last_debounce = 0;
bool is_wifi_on = false;

// Non-blocking Timers
unsigned long last_1sec_timer = 0;
unsigned long last_battery_timer = 0;
unsigned long last_wifi_sync_timer = 0;

// ============================================================
// SENSOR STATE / STORED VALUES
// ============================================================
int stored_hr = 0;
float stored_temp = 0.0;
int stored_hum = 0;
int stored_bp_sys = 0;
int stored_bp_dia = 0;
int stored_battery = 0;

// ============================================================
// UI HELPER FUNCTIONS
// ============================================================
/* LVGL Display Flush */
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  
  lv_disp_flush_ready(disp_drv);
}

/* LVGL Touch Read (WITH ANTI-GHOST TOUCH SAFETY) */
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
  if (!is_screen_on) {
    data->state = LV_INDEV_STATE_RELEASED; // Safety trap to prevent LVGL lockups
    return; 
  }
  
  if (touch.available()) {
    data->state = LV_INDEV_STATE_PRESSED; 
    data->point.x = touch.data.x;
    data->point.y = touch.data.y;
    
    // Debug Touch (Fires every 500ms while held to avoid completely flooding the monitor)
    static unsigned long last_touch_print = 0;
    if (millis() - last_touch_print > 500) {
        Serial.printf("DEBUG: Touch detected at X:%d Y:%d\n", touch.data.x, touch.data.y);
        last_touch_print = millis();
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED; 
  }
}

/* Reusable button UI updater */
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
// SENSOR FUNCTIONS (Event Callbacks)
// ============================================================
void start_hr_measurement(lv_event_t * e) {
  if (hr_state != MEASURING) {
    Serial.println("DEBUG: Heart Rate Measurement STARTED.");
    hr_state = MEASURING;
    hr_meas_start = millis();
    hr_valid_reads = 0; // Reset stabilization counter
    start_HEART(); 
    set_button_state(ui_buttonHeart, ui_StartMeasure2, true, 0xFA4040, 0x880000);
  }
}

void start_temp_measurement(lv_event_t * e) {
  if (temp_state != MEASURING) {
    Serial.println("DEBUG: Temp/Hum Measurement STARTED.");
    temp_state = MEASURING;
    temp_meas_start = millis();
    last_temp_poll = 0; // Force immediate initial read
    set_button_state(ui_buttonTemp, ui_StartMeasure5, true, 0xB25100, 0x602000);
  }
}

void start_bp_measurement(lv_event_t * e) {
  if (bp_state != MEASURING) {
    Serial.println("DEBUG: Blood Pressure Measurement STARTED.");
    bp_state = MEASURING;
    bp_meas_start = millis();
    init_BP(); 
    set_button_state(ui_buttonBP, ui_StartMeasure4, true, 0x007E79, 0x004040);
  }
}

// ============================================================
// BATTERY
// ============================================================
void update_battery() {
  Data_BATTERY b = readBATTERY();
  stored_battery = b.percentage;
  Serial.printf("DEBUG: Battery Updated: %d%%\n", stored_battery);
}

// ============================================================
// RTC / CLOCK
// ============================================================
void update_clock_ui() {
  RTC_Data t = readRTC();
  
  // 1-Hour Network Sync
  if (is_wifi_on && WiFi.status() == WL_CONNECTED) {
    if (millis() - last_wifi_sync_timer >= 3600000) { 
      Serial.println("DEBUG: Performing 1-Hour NTP Time Sync...");
      syncTimeRTC();
      last_wifi_sync_timer = millis();
    }
  }

  // Update Homepage
  if (lv_scr_act() == ui_Screen1) {
    lv_label_set_text_fmt(ui_time, "%02d:%02d", t.hour, t.minute);
    lv_label_set_text_fmt(ui_date, "%02d", t.day);
    
    if (stored_hr > 0) {
      lv_label_set_text_fmt(ui_lastHeartRate, "%d", stored_hr);
      lv_arc_set_value(ui_heartRateArc, stored_hr);
    }
    if (stored_temp > 0.0) {
      String tempStr = String(stored_temp, 1);
      lv_label_set_text_fmt(ui_lastBodyTempValue, "%s°C", tempStr.c_str());
      lv_label_set_text_fmt(ui_lastHumidityValue, "%d%%", stored_hum);
    }
    if (stored_battery > 0) {
      lv_label_set_text_fmt(ui_batteryPercentage, "%d", stored_battery);
      lv_arc_set_value(ui_batteryArc, stored_battery);
    }
    
    // SpO2 Placeholder
    lv_label_set_text(ui_lastSpO2Value, "--");
    lv_arc_set_value(ui_spO2Arc, 0);
  }
}

// ============================================================
// WIFI
// ============================================================
void wifi_toggle(lv_event_t * e) {
  lv_obj_t * sw = lv_event_get_target(e);
  is_wifi_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  
  if (is_wifi_on) {
    Serial.println("DEBUG: Wi-Fi toggled ON.");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS); // Uses config.h variables
  } else {
    Serial.println("DEBUG: Wi-Fi toggled OFF.");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
  }
}

// ============================================================
// BUTTON (Hardware Toggle)
// ============================================================
void handle_hardware_button() {
  int reading = digitalRead(BUTTON_PIN); // Button Pin 1 from config.h
  
  if (reading != last_button_val) {
    last_debounce = millis();
  }
  
  if ((millis() - last_debounce) > 50) {
    if (reading != button_state) {
      button_state = reading;
      
      if (button_state == LOW) { // Button Pressed
        is_screen_on = !is_screen_on;
        digitalWrite(TFT_BL_PIN, is_screen_on ? HIGH : LOW);
        Serial.printf("DEBUG: Hardware Button Pressed! Screen is now %s\n", is_screen_on ? "ON" : "OFF");
      }
    }
  }
  last_button_val = reading;
}

// ============================================================
// DISPLAY BRIGHTNESS
// FUTURE INTEGRATION:
//
// A brightness slider/bar will be added to the SquareLine UI.
// Once the slider exists, connect its value-change event to the
// display brightness function here.
//
// DO NOT uncomment or implement the UI event until the actual
// SquareLine brightness object has been added.
// ============================================================

// ============================================================
// MAIN SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n===============================");
  Serial.println("SYSTEM BOOTING...");
  Serial.println("===============================");
  
  // Hardware - Button Pin 1 configured with internal pullup
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);

  // Initialize Sensors
  Serial.println("DEBUG: Initializing Sensors...");
  init_RTC();
  init_AHT10();
  init_BATTERY();
  init_HEART();
  stop_HEART(); // Immediately idle HR sensor on boot

  // Display & Touch
  Serial.println("DEBUG: Initializing Display & Touch...");
  tft.begin();
  tft.setRotation(0); 
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
  
  // THE FIX: Start the touch controller!
  touch.begin();
  
  // LVGL Init
  Serial.println("DEBUG: Booting LVGL Graphics Engine...");
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
  
  // Load UI
  Serial.println("DEBUG: Loading SquareLine UI...");
  ui_init();
  current_screen = ui_Screen1;

  // Event Callbacks (Bind UI safely)
  lv_obj_add_event_cb(ui_buttonHeart, start_hr_measurement, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(ui_buttonTemp, start_temp_measurement, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(ui_buttonBP, start_bp_measurement, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(ui_wifiSwitch, wifi_toggle, LV_EVENT_VALUE_CHANGED, NULL);

  // Initial Battery Read
  update_battery();

  Serial.println("===============================");
  Serial.println("SYSTEM BOOT COMPLETE!");
  Serial.println("===============================");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  lv_timer_handler(); 
  lv_tick_inc(5); 

  handle_hardware_button();

  if (!is_screen_on) {
    delay(50);
    return; // Pause processing when screen is asleep
  }

  // ============================================================
  // SCREEN CHANGE CANCELLATION TRAP
  // ============================================================
  lv_obj_t* act_scr = lv_scr_act();
  if (current_screen != act_scr) {
    Serial.println("DEBUG: Screen Swiped/Changed!");
    
    // If the user navigated away while measuring, cancel it immediately!
    if (current_screen == ui_Screen2 && hr_state == MEASURING) {
      Serial.println("DEBUG: HR Cancelled by screen change.");
      stop_HEART();
      hr_state = IDLE;
      set_button_state(ui_buttonHeart, ui_StartMeasure2, false, 0xFA4040, 0);
      lv_label_set_text(ui_heartRateLive, "--");
    }
    else if (current_screen == ui_Screen4 && bp_state == MEASURING) {
      Serial.println("DEBUG: BP Cancelled by screen change.");
      stop_BP();
      bp_state = IDLE;
      set_button_state(ui_buttonBP, ui_StartMeasure4, false, 0x007E79, 0);
      lv_label_set_text(ui_sysLive, "--");
      lv_label_set_text(ui_diaLive, "--");
    }
    else if (current_screen == ui_Screen5 && temp_state == MEASURING) {
      Serial.println("DEBUG: Temp Cancelled by screen change.");
      temp_state = IDLE;
      set_button_state(ui_buttonTemp, ui_StartMeasure5, false, 0xB25100, 0);
      lv_label_set_text(ui_tempLive, "--");
      lv_label_set_text(ui_humLive, "--");
    }

    // Trigger the 60-second idle timer for the screen they just left
    timed_out_screen = current_screen;
    screen_left_timer = millis();
    screen_timeout_active = true;
    current_screen = act_scr;
  }

  // ============================================================
  // SENSOR STATE MACHINES (With Stabilization & Timeouts)
  // ============================================================
  
  // --- HEART RATE (12-Second Stabilization Window) ---
  if (hr_state == MEASURING) {
    int bpm = readHEART();
    unsigned long elapsed = millis() - hr_meas_start;

    // Grace period: allow values to fluctuate and stabilize
    if (bpm > 40 && bpm < 220) { 
      hr_valid_reads++;
      stored_hr = bpm; // Continuously update to the latest value
      lv_label_set_text_fmt(ui_heartRateLive, "%d", bpm); // Live fluctuating UI
      
      // Limit serial printing so we don't spam the console
      static unsigned long last_hr_print = 0;
      if (millis() - last_hr_print > 1000) {
         Serial.printf("DEBUG: Live HR fluctuating: %d BPM\n", bpm);
         last_hr_print = millis();
      }
    } 

    // Finalize after 12 seconds
    if (elapsed > 12000) {
      stop_HEART();
      
      if (hr_valid_reads > 0) { // We captured a real finger!
        Serial.printf("DEBUG: HR Measurement COMPLETE. Final: %d BPM\n", stored_hr);
        hr_state = COMPLETE;
        lv_label_set_text_fmt(ui_heartRateLive, "%d", stored_hr);
      } else { // 12 seconds passed and it only saw -1
        Serial.println("DEBUG: HR ERROR. No finger detected during window.");
        hr_state = SENSOR_ERROR;
        lv_label_set_text(ui_heartRateLive, "Err");
      }
      set_button_state(ui_buttonHeart, ui_StartMeasure2, false, 0xFA4040, 0);
    }
  }

  // --- TEMPERATURE & HUMIDITY (6-Second Stabilization Window) ---
  if (temp_state == MEASURING) {
    unsigned long elapsed = millis() - temp_meas_start;

    // Poll live data every 1 second during the window
    if (millis() - last_temp_poll > 1000) {
      AHT10_Data clim = readAHT10();
      if (clim.temperature != 0.0) {
        stored_temp = clim.temperature;
        stored_hum = (int)clim.humidity;
        
        String tempStr = String(stored_temp, 1);
        lv_label_set_text_fmt(ui_tempLive, "%s°C", tempStr.c_str());
        lv_label_set_text_fmt(ui_humLive, "%d%%", stored_hum);
        Serial.printf("DEBUG: Live Temp/Hum: %s°C, %d%%\n", tempStr.c_str(), stored_hum);
      }
      last_temp_poll = millis();
    }

    // Finalize after 6 seconds of stabilization
    if (elapsed > 6000) {
      Serial.println("DEBUG: Temp/Hum Measurement COMPLETE.");
      temp_state = COMPLETE;
      set_button_state(ui_buttonTemp, ui_StartMeasure5, false, 0xB25100, 0);
    }
  }

  // --- BLOOD PRESSURE (30-Second UART Timeout) ---
  if (bp_state == MEASURING) {
    unsigned long elapsed = millis() - bp_meas_start;
    BP_Data bp = readBP();

    if (bp.status == 'v') { // Valid reading received
      stored_bp_sys = bp.sys;
      stored_bp_dia = bp.dia;
      stop_BP();
      bp_state = COMPLETE;
      set_button_state(ui_buttonBP, ui_StartMeasure4, false, 0x007E79, 0);
      lv_label_set_text_fmt(ui_sysLive, "%d mmHg", stored_bp_sys);
      lv_label_set_text_fmt(ui_diaLive, "%d mmHg", stored_bp_dia);
      Serial.printf("DEBUG: BP Measurement COMPLETE. Final: %d/%d mmHg\n", stored_bp_sys, stored_bp_dia);
    } 
    // Error, turned off, or 30 second Hard Timeout waiting for cuff
    else if (bp.status == 'e' || bp.status == 'o' || elapsed > 30000) { 
      stop_BP();
      bp_state = SENSOR_ERROR;
      set_button_state(ui_buttonBP, ui_StartMeasure4, false, 0x007E79, 0);
      lv_label_set_text(ui_sysLive, "Err");
      lv_label_set_text(ui_diaLive, "Err");
      Serial.println("DEBUG: BP ERROR or 30-Second Timeout reached.");
    }
  }

  // ============================================================
  // SpO2 MEASUREMENT
  // Future implementation.
  // When the SpO2 sensor library is added, follow the same
  // start -> measure -> final result -> stop sensor architecture.
  // ============================================================

  // ============================================================
  // 60-SECOND IDLE SCREEN BLANKING
  // ============================================================
  if (screen_timeout_active && (millis() - screen_left_timer > 60000)) {
    
    if (timed_out_screen == ui_Screen2 && hr_state != MEASURING) {
      Serial.println("DEBUG: 60s Timeout - Clearing HR UI.");
      lv_label_set_text(ui_heartRateLive, "--");
    } 
    else if (timed_out_screen == ui_Screen5 && temp_state != MEASURING) {
      Serial.println("DEBUG: 60s Timeout - Clearing Temp/Hum UI.");
      lv_label_set_text(ui_tempLive, "--");
      lv_label_set_text(ui_humLive, "--");
    }
    // NOTE: Blood Pressure (ui_Screen4) is intentionally excluded from clearing!
    
    screen_timeout_active = false;
  }

  // ============================================================
  // PERIODIC TASKS
  // ============================================================
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

/*
#include "config.h"
#include <Wire.h>
#include "temperature.h"
#include "heart_rate.h"
#include "rtc.h"
#include "BP.h"
#include "battery.h"

void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // Starting delay
    delay(500);
    Serial.println("Initializing AHT10 Temp Sensor... ");
    init_AHT10();

    Serial.println("Initializing MAX30102 Heart Sensor... ");
    init_HEART();

    Serial.println("Initializing Real Time Clock... ");
    init_RTC();

    Serial.println("Initializing Blood Pressure Monitor... ");
    init_BP();  //Turn on/start hearing on RX pin 44

    Serial.println("Initializing Battery Measuring Code... ");
    init_BATTERY();

}



void loop()

{

  //This is how you can access data from the sensors once they are initialised successfully
  AHT10_Data live = readAHT10();
  Serial.println(live.humidity);
  Serial.println(live.temperature);
  delay(500);

  live_bpm = readHEART();
  Serial.println(live_bpm);

  RTC_Data time = readRTC();
  Serial.println(time.second);
  Serial.println(time.minute);
  delay(1000);

  BP_Data currentBP = readBP();
  if(currentBP.status == 'w'){
      // do nothing;
  }
  else if(currentBP.status == 's'){
    Serial.println("Started :) Please Stay Calm");
  }
  else if(currentBP.status == 'e'){
    Serial.println("Error 2,4,6");
  }
  else if(currentBP.status == 'v'){
  Serial.println(currentBP.sys);  //systolic pressure
  Serial.println(currentBP.dia);  //diastolic pressure
  Serial.println(currentBP.heart);  //heart beat
  Serial.println(currentBP.status); //For debugging check BP.h
  }

  else if(currentBP.status == 'o'){
    stop_BP(); //Turn off RX pin 44
  }

  Data_BATTERY live_battery = readBATTERY();
  Serial.println(live_battery.batt_voltage);  //battery voltage
  Serial.println(live_battery.percentage);  //battery capacity(%)
  delay(1000);
}
*/