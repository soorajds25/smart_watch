#ifndef API_SYNC_H
#define API_SYNC_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <ArduinoJson.h> 
#include <Preferences.h> 

extern int stored_bp_hr; 
void show_toast(const char* msg);

const char* cloudApiUrl = "https://patient-health-monitoring-pi.vercel.app/api/entry";
const char* DEFAULT_DEVICE_TOKEN = "pdk_8300882366a973c0d8669c4b13c9a2031ae01ed9bef98f76"; 
String DEVICE_TOKEN = "";

void initDeviceCredentials() {
  Preferences prefs;
  prefs.begin("dev_config", true);
  DEVICE_TOKEN = prefs.getString("token", DEFAULT_DEVICE_TOKEN);
  prefs.end();
  Serial.print("DEBUG: [NVS] Loaded Device Token: ");
  Serial.println(DEVICE_TOKEN);
}

void saveDeviceTokenToNVS(const char* newToken) {
  Preferences prefs;
  prefs.begin("dev_config", false);
  prefs.putString("token", newToken);
  prefs.end();
  DEVICE_TOKEN = String(newToken);
  Serial.println("DEBUG: [NVS] Saved new token to flash NVS successfully!");
}

bool sendReadingToCloud(StaticJsonDocument<128>& jsonDoc) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (DEVICE_TOKEN.length() == 0) {
    initDeviceCredentials();
  }

  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;
  http.begin(client, cloudApiUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + DEVICE_TOKEN);

  String requestBody;
  serializeJson(jsonDoc, requestBody);
  int httpCode = http.POST(requestBody);
  
  bool success = false;
  if (httpCode > 0) {
    String responseStr = http.getString();
    
    if (responseStr.indexOf("\"new_token\"") > 0) {
      StaticJsonDocument<256> respDoc;
      if (!deserializeJson(respDoc, responseStr) && respDoc.containsKey("new_token")) {
        saveDeviceTokenToNVS(respDoc["new_token"]);
        show_toast("User Assigned!"); 
      }
    }

    if (httpCode == 200 || httpCode == 201) {
      success = true;
    }
  } 
  
  http.end();
  return success;
}

bool performCloudSync(int hr, float temp, int hum, int bp_sys, int bp_dia, int spo2) {
  bool overallSuccess = true;

  if (bp_sys > 0 && bp_dia > 0) {
    StaticJsonDocument<128> sysDoc, diaDoc;
    sysDoc["bp_value"] = bp_sys; sysDoc["bp_type"] = "systolic";
    diaDoc["bp_value"] = bp_dia; diaDoc["bp_type"] = "diastolic";
    if (!sendReadingToCloud(sysDoc) || !sendReadingToCloud(diaDoc)) overallSuccess = false;

    if (stored_bp_hr > 0) {
        StaticJsonDocument<128> bpHrDoc;
        bpHrDoc["heart_rate"] = stored_bp_hr; bpHrDoc["source"] = "bp_monitor"; 
        if (!sendReadingToCloud(bpHrDoc)) overallSuccess = false;
    }
  }

  if (hr > 0) {
    StaticJsonDocument<128> hrDoc; hrDoc["heart_rate"] = hr; 
    if (!sendReadingToCloud(hrDoc)) overallSuccess = false;
  }

  if (temp > 0.0) {
    StaticJsonDocument<128> tempDoc; tempDoc["temp"] = temp; 
    if (!sendReadingToCloud(tempDoc)) overallSuccess = false;
  }

  if (hum > 0) {
    StaticJsonDocument<128> humDoc; humDoc["humidity"] = hum; 
    if (!sendReadingToCloud(humDoc)) overallSuccess = false;
  }

  if (spo2 > 0) {
    StaticJsonDocument<128> spo2Doc; spo2Doc["spo2"] = spo2; 
    if (!sendReadingToCloud(spo2Doc)) overallSuccess = false;
  }

  return overallSuccess;
}

#endif // API_SYNC_H