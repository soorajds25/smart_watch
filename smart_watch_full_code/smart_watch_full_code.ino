#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Wire.h>
#include <CST816S.h>
#include "ui.h" 

#define TFT_BL 2 
#define TP_SDA 11
#define TP_SCL 12
#define TP_INT 13
#define TP_RST 8

TFT_eSPI tft = TFT_eSPI();
CST816S touch(TP_SDA, TP_SCL, TP_RST, TP_INT);

/* 1. Display flushing function for LVGL */
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  
  // Custom FPS Tracker
  static uint32_t last_time = 0;
  static uint32_t frames = 0;
  frames++;
  if (millis() - last_time >= 1000) {
      Serial.print("Live FPS: ");
      Serial.println(frames);
      frames = 0;
      last_time = millis();
  }

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true); // Reverted to standard push
  tft.endWrite();
  
  lv_disp_flush_ready(disp_drv);
}

/* 2. Touch reading function for LVGL */
void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
  if (touch.available()) {
    data->state = LV_INDEV_STATE_PR; 
    data->point.x = touch.data.x;
    data->point.y = touch.data.y;
  } else {
    data->state = LV_INDEV_STATE_REL; 
  }
}

void setup() {
  Serial.begin(115200);
  
  tft.begin();
  tft.setRotation(0); 
  // initDMA() removed
  
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  Wire.begin(TP_SDA, TP_SCL); 
  touch.begin();
  
  lv_init();
  
  // Reverted to standard array allocation to prevent heap crashes
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
}

void loop() {
  lv_timer_handler(); 
  delay(5);
  lv_tick_inc(5); 
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