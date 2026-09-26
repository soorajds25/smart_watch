#ifndef API_SYNC_H
#define API_SYNC_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> // <--- CRITICAL FIX: ESP32 secure client library
#include <ArduinoJson.h> 

// ==========================================
// CLOUD API CONFIGURATION
// ==========================================
const char* cloudApiUrl = "https://patient-health-monitoring-pi.vercel.app/api/entry";

// NOTE: Replace this with your authentic token from the dashboard
const String DEVICE_TOKEN = "pdk_a9faab960e6b1f2e8df393e349ea467948aeaced1a1618e7"; 

// Helper function to serialize and send JSON
bool sendReadingToCloud(StaticJsonDocument<128>& jsonDoc) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("DEBUG: [API] Sync Failed - No Wi-Fi");
    return false;
  }

  // CRITICAL FIX: Set up the secure client to ignore SSL verification
  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  
  // Pass the secure client into the HTTP client
  http.begin(client, cloudApiUrl);
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + DEVICE_TOKEN);

  String requestBody;
  serializeJson(jsonDoc, requestBody);
  
  Serial.println("DEBUG: [API] Sending: " + requestBody);
  int httpCode = http.POST(requestBody);
  
  bool success = false;
  if (httpCode == 201) {
    Serial.println("DEBUG: [API] Cloud Sync SUCCESS (201 Created)");
    success = true;
  } else if (httpCode == 422) {
    Serial.println("DEBUG: [API] FAILED (422) -> Type Mismatch. Check API Docs.");
  } else {
    Serial.printf("DEBUG: [API] FAILED. HTTP Code: %d\n", httpCode);
  }
  
  http.end();
  return success;
}

// Master function triggered by the "Sync Now" button
// Added spo2 as the LAST parameter to prevent main loop crashes
bool performCloudSync(int hr, float temp, int hum, int bp_sys, int bp_dia, int spo2) {
  Serial.println("\nDEBUG: [API] --- EVALUATING DATA FOR CLOUD SYNC ---");
  bool overallSuccess = true;
  bool dataSent = false;

  // 1. BLOOD PRESSURE (Documented)
  if (bp_sys > 0 && bp_dia > 0) {
    Serial.println("DEBUG: [API] Valid BP found. Uploading...");
    
    StaticJsonDocument<128> sysDoc;
    sysDoc["bp_value"] = bp_sys;
    sysDoc["bp_type"] = "systolic";
    if (!sendReadingToCloud(sysDoc)) overallSuccess = false;
    
    StaticJsonDocument<128> diaDoc;
    diaDoc["bp_value"] = bp_dia;
    diaDoc["bp_type"] = "diastolic";
    if (!sendReadingToCloud(diaDoc)) overallSuccess = false;
    
    dataSent = true;
  } else {
    Serial.println("DEBUG: [API] Skipping BP -> Value is 0");
  }

  // 2. HEART RATE (Educated Guess from Dashboard UI)
  if (hr > 0) {
    Serial.println("DEBUG: [API] Valid Heart Rate found. Uploading...");
    StaticJsonDocument<128> hrDoc;
    hrDoc["heart_rate"] = hr; 
    if (!sendReadingToCloud(hrDoc)) overallSuccess = false;
    dataSent = true;
  } else {
    Serial.println("DEBUG: [API] Skipping Heart Rate -> Value is 0");
  }

  // 3. TEMPERATURE (Educated Guess from Dashboard UI)
  if (temp > 0.0) {
    Serial.println("DEBUG: [API] Valid Temperature found. Uploading...");
    StaticJsonDocument<128> tempDoc;
    tempDoc["temp"] = temp; 
    if (!sendReadingToCloud(tempDoc)) overallSuccess = false;
    dataSent = true;
  } else {
    Serial.println("DEBUG: [API] Skipping Temperature -> Value is 0");
  }

  // 4. HUMIDITY (Educated Guess from Dashboard UI)
  if (hum > 0) {
    Serial.println("DEBUG: [API] Valid Humidity found. Uploading...");
    StaticJsonDocument<128> humDoc;
    humDoc["humidity"] = hum; 
    if (!sendReadingToCloud(humDoc)) overallSuccess = false;
    dataSent = true;
  } else {
    Serial.println("DEBUG: [API] Skipping Humidity -> Value is 0");
  }

  // 5. SpO2 (Documented)
  if (spo2 > 0) {
    Serial.println("DEBUG: [API] Valid SpO2 found. Uploading...");
    StaticJsonDocument<128> spo2Doc;
    spo2Doc["spo2"] = spo2; 
    if (!sendReadingToCloud(spo2Doc)) overallSuccess = false;
    dataSent = true;
  } else {
    Serial.println("DEBUG: [API] Skipping SpO2 -> Value is 0");
  }

  if (!dataSent) {
    Serial.println("DEBUG: [API] Sync Aborted -> No valid data to upload.");
  } else {
    Serial.println("DEBUG: [API] --- CLOUD SYNC COMPLETE ---");
  }

  return overallSuccess;
}

#endif // API_SYNC_H