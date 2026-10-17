#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

String g_wifi_ssid = "";
String g_wifi_pass = "";
bool is_provisioning = false;
bool pending_portal_stop = false;

WebServer server(80);
DNSServer dnsServer;

const char* portal_html = 
  "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
  "<style>body{font-family:Arial;padding:20px;text-align:center;background:#1e1e1e;color:#fff;}"
  "input{width:100%;padding:12px;margin:10px 0;box-sizing:border-box;border-radius:6px;border:none;font-size:16px;}"
  "button{background:#10C400;color:white;padding:15px;width:100%;border:none;border-radius:6px;font-size:18px;font-weight:bold;margin-top:15px;}</style></head>"
  "<body><h2>Watch Wi-Fi Setup</h2>"
  "<form action='/save' method='POST'>"
  "<input type='text' name='ssid' placeholder='Home Wi-Fi Name' required><br>"
  "<input type='password' name='pass' placeholder='Password (Optional)'><br>"
  "<button type='submit'>Save & Connect</button>"
  "</form></body></html>";

void loadWiFiCredentials() {
  Preferences prefs;
  prefs.begin("dev_config", true);
  g_wifi_ssid = prefs.getString("wifi_ssid", "");
  g_wifi_pass = prefs.getString("wifi_pass", "");
  prefs.end();
  Serial.printf("DEBUG: [NVS] Loaded Wi-Fi SSID: %s\n", g_wifi_ssid.c_str());
}

void saveWiFiCredentials(String ssid, String pass) {
  Preferences prefs;
  prefs.begin("dev_config", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  prefs.end();
  g_wifi_ssid = ssid;
  g_wifi_pass = pass;
  Serial.printf("DEBUG: [NVS] Saved New Wi-Fi SSID: %s\n", ssid.c_str());
}

void handleRoot() {
  server.send(200, "text/html", portal_html);
}

void handleSave() {
  if (server.hasArg("ssid")) {
    saveWiFiCredentials(server.arg("ssid"), server.arg("pass"));
    server.send(200, "text/html", "<html><body style='font-family:Arial;text-align:center;background:#1e1e1e;color:#10C400;padding:50px;'><h2>Wi-Fi Saved!</h2><p>You can close this window. The watch is reconnecting...</p></body></html>");
    pending_portal_stop = true; 
  }
}

void startWiFiProvisioningPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Smartwatch-Setup"); 
  dnsServer.start(53, "*", WiFi.softAPIP()); 
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot); 
  server.begin();
  
  is_provisioning = true;
  Serial.println("DEBUG: [PORTAL] Wi-Fi Hotspot Started (Smartwatch-Setup)");
}

void stopWiFiProvisioningPortal() {
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  is_provisioning = false;
  pending_portal_stop = false;
  Serial.println("DEBUG: [PORTAL] Wi-Fi Hotspot Stopped");
}

void serviceWiFiPortal() {
  if (is_provisioning) {
    dnsServer.processNextRequest();
    server.handleClient();
  }
}

#endif // WIFI_PORTAL_H