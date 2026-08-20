#include "config.h"
#include <Wire.h>
#include "temperature.h"
#include "heart_rate.h"
#include "rtc.h"

void setup() {
    Serial.begin(115200);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // Starting delay
    delay(500);

    Serial.print("Initializing AHT10 Temp Sensor... ");
    init_AHT10();

    Serial.print("Initializing MAX30102 Heart Sensor... ");
    init_HEART();

    Serial.print("Initializing Real Time Clock... ");
    init_RTC();

}

void loop()
{
  //This is how you can access data from the sensors once they are initialised successfully

  // AHT10_Data live = readAHT10();
  // Serial.println(live.humidity);
  // Serial.println(live.temperature);
  // delay(500);

  // live_bpm = readHEART();
  // Serial.println(live_bpm);

  // RTC_Data time = readRTC();
  // Serial.println(time.second);
  // Serial.println(time.minute);
  // delay(1000);
}