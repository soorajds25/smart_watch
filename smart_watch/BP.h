#ifndef BP_H
#define BP_H

#include <Arduino.h>

#define BP_RX_PIN 44 // Default RX pin near the USB-C port on ESP32-S3 Zero 
#define BP_TX_PIN 43 // Default TX pin (unused, but mapped to board layout)

// 1. Data Structure
struct BP_Data {
  int sys;
  int dia;
  int heart; 
  char status; // 'w'=waiting, 's'=start, 'f'=finished, 'e'=error, 'o'=off, 'v'=valid reading
};

// 2. Global state variables for the BP monitor
String bp_currentLine = "";
bool bp_expecting_result = false;

// 3. Setup Function 
inline bool init_BP() {
  // Initialize Serial1 using the default hardware pins
  Serial1.begin(115200, SERIAL_8N1, BP_RX_PIN, BP_TX_PIN);
  
  // Serial1.begin doesn't return a boolean, so if the board initializes, it's true.
  return true; 
}

// --- Helper Logic for Parsing (Internal to this file) ---
inline BP_Data processMonitorData(String line) {
  BP_Data result = {0, 0, 0, 'w'}; // Default state: waiting

  if (line.length() == 0) return result;

  // Detect Start of Measurement
  if (line.indexOf("startBPTest") != -1) {
    result.status = 's';
    return result;
  }
  
  // Detect End of Measurement & Errors
  if (line.indexOf("end test") != -1) {
    if (line.indexOf("err:2") != -1 || line.indexOf("err:4") != -1 || line.indexOf("err:6") != -1) {
      result.status = 'e'; // Error detected
    } else {
      result.status = 'f'; // Finished pumping
    }
    return result;
  }

  // Detect Power Down
  if (line.indexOf("MSG_POWER_DOWN") != -1) {
    result.status = 'o';
    return result;
  }

  // Detect the trigger for the final result payload
  if (line.indexOf("save record") != -1) {
    bp_expecting_result = true;
    return result;
  }

  // Parse the Hex Payload (e.g., "93 5C 82 4A ...")
  if (bp_expecting_result) {
    int sys, dia, map_val, hr;
    int parsed = sscanf(line.c_str(), "%x %x %x %x", &sys, &dia, &map_val, &hr);
    
    if (parsed >= 4) {
      result.sys = sys;
      result.dia = dia;
      result.heart = hr;
      result.status = 'v'; // 'v' for Valid data captured!
    }
    bp_expecting_result = false; // Reset the flag
  }
  
  return result;
}

// 4. Read Function 
inline BP_Data readBP() {
  BP_Data data = {0, 0, 0, 'w'}; // Start with waiting state

  // Read incoming data continuously without blocking
  while (Serial1.available()) {
    char c = Serial1.read();
    
    if (c == '\n') {
      bp_currentLine.trim(); 
      data = processMonitorData(bp_currentLine);
      bp_currentLine = ""; // Reset buffer
      
      // If a major event happened (data valid, error, turn off), return it immediately
      if (data.status != 'w') {
        return data;
      }
    } else if (c != '\r') {
      bp_currentLine += c; 
    }
  }
  
  return data;
}

inline void stop_BP() {
  Serial1.end();
  Serial.println("BP Monitor UART channel shut down to save power.");
}

#endif