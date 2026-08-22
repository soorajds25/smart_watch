#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>

#define BATTERY_ADC_PIN 3

// Resistor values in Ohms
const float R1 = 10000.0; // Top resistor connected to VOUT+
const float R2 = 15000.0; // Bottom resistor connected to GND

// The multiplier to reverse the voltage divider math
// (10k + 15k) / 15k = 1.66667
// const float VOLTAGE_DIVIDER_RATIO = (R1 + R2) / R2;

// Custom calibrated multiplier (Multimeter Voltage / ADC Pin Voltage)
// 4.09V / 2.41V = 1.697
const float VOLTAGE_DIVIDER_RATIO = 1.697;

struct Data_BATTERY{
  float pin_voltage;
  float batt_voltage;
  int percentage;
};

inline void init_BATTERY() {  
  // Set the ADC attenuation to 11dB (allows reading up to ~3.1V safely)
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  Serial.println("LiPo Battery Monitor Started....");
}

/**
 * Calculates battery percentage using a polynomial curve matched
 * to standard 3.7V (4.2V Max) LiPo discharge profiles.
 */

inline int calculateBatteryPercentage(double v) {
  // Polynomial equation from reference
  double y = -144.9390 * v * v * v
           + 1655.8629 * v * v
           - 6158.8520 * v
           + 7501.3202;

  // Enforce absolute bounds so you don't get 105% or -10%
  if (y > 100.0) y = 100.0;
  if (y < 0.0) y = 0.0;
  // Round to nearest integer
  return (int)round(y);
}

inline Data_BATTERY readBATTERY() {
  Data_BATTERY data;

  // 1. Read factory-calibrated millivolts directly from the ESP32 API
  uint32_t adc_mv = analogReadMilliVolts(BATTERY_ADC_PIN);

  // 2. Convert millivolts to volts for the pin
  data.pin_voltage = adc_mv / 1000.0;

  // 3. Multiply by the divider ratio to get the actual battery voltage
  data.batt_voltage = data.pin_voltage * VOLTAGE_DIVIDER_RATIO;

  // 4. Pass the real voltage into the polynomial curve function
  data.percentage = calculateBatteryPercentage(data.batt_voltage);
  return data;
}



#endif









//Original Code(TESTED and WORKING)

/*

#include <Arduino.h>



#define BATTERY_ADC_PIN 3



// Resistor values in Ohms (from your schematic)

const float R1 = 10000.0; // Top resistor connected to VOUT+

const float R2 = 15000.0; // Bottom resistor connected to GND



// The multiplier to reverse the voltage divider math

// (10k + 15k) / 15k = 1.66667

// const float VOLTAGE_DIVIDER_RATIO = (R1 + R2) / R2;



// Custom calibrated multiplier (Multimeter Voltage / ADC Pin Voltage)

// 4.09V / 2.41V = 1.697

const float VOLTAGE_DIVIDER_RATIO = 1.697;



void setup() {

  Serial.begin(115200);

 

  // Set the ADC attenuation to 11dB (allows reading up to ~3.1V safely)

  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

 

  Serial.println("====================================");

  Serial.println("  LiPo Battery Monitor Started      ");

  Serial.println("====================================\n");

}



/**

 * Calculates battery percentage using a polynomial curve matched

 * to standard 3.7V (4.2V Max) LiPo discharge profiles.



int calculateBatteryPercentage(double v) {

  // Polynomial equation from reference

  double y = -144.9390 * v * v * v

           + 1655.8629 * v * v

           - 6158.8520 * v

           + 7501.3202;



  // Enforce absolute bounds so you don't get 105% or -10%

  if (y > 100.0) y = 100.0;

  if (y < 0.0) y = 0.0;

 

  // Round to nearest integer

  return (int)round(y);

}



void loop() {

  // 1. Read factory-calibrated millivolts directly from the ESP32 API

  uint32_t adc_mv = analogReadMilliVolts(BATTERY_ADC_PIN);

 

  // 2. Convert millivolts to volts for the pin

  float pin_voltage = adc_mv / 1000.0;

 

  // 3. Multiply by the divider ratio to get the actual battery voltage

  float batt_voltage = pin_voltage * VOLTAGE_DIVIDER_RATIO;

 

  // 4. Pass the real voltage into the polynomial curve function

  int percentage = calculateBatteryPercentage(batt_voltage);



  // 5. Print out the diagnostics

  Serial.print("ADC Pin: ");

  Serial.print(pin_voltage, 2);

  Serial.print("V  |  ");

 

  Serial.print("True Battery: ");

  Serial.print(batt_voltage, 2);

  Serial.print("V  |  ");

 

  Serial.print("Capacity: ");

  Serial.print(percentage);

  Serial.println("%");

 

  delay(2000);

}

*/