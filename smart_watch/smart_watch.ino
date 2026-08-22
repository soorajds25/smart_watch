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