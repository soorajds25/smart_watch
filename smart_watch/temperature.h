#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>

// 1. Data Structure
struct AHT10_Data {
    float temperature;
    float humidity;
};

// 2. Global instance of the sensor
Adafruit_AHTX0 aht;

// 3. Setup Function
inline bool init_AHT10() {
  // The begin() function comes from the Adafruit library
  if (!aht.begin()) {
    Serial.println("Could not find AHT10? Check wiring");
    return false; // Bails out safely instead of freezing
  }
  Serial.println("AHT10 found");
  return true;
}

// 4. Read Function
inline AHT10_Data readAHT10() {
    AHT10_Data data;
    sensors_event_t humidity, temp;
   
    aht.getEvent(&humidity, &temp); // Fetching data using the library

    data.temperature = temp.temperature;
    data.humidity = humidity.relative_humidity;
    return data;
}


#endif





/*  Original Example Code (TESTED and WORKING)

#include <Adafruit_AHTX0.h>



Adafruit_AHTX0 aht;



void setup() {

  Serial.begin(115200);

  Serial.println("Adafruit AHT10 demo!");



  Wire.begin(11,12);



  if (! aht.begin()) {

    Serial.println("Could not find AHT10? Check wiring");

    while (1) delay(10);

  }

  Serial.println("AHT10 found");

}



void loop() {

  sensors_event_t humidity, temp;

  aht.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data

  Serial.print("Temperature: "); Serial.print(temp.temperature); Serial.println(" degrees C");

  Serial.print("Humidity: "); Serial.print(humidity.relative_humidity); Serial.println("% rH");



  delay(500);

}

*/