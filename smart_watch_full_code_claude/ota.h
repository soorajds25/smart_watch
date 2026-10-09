#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

// ==========================================
// OTA UPDATE CONFIGURATION
// ==========================================

// 1. Current Firmware Version 
// -> BUMP THIS NUMBER UP BEFORE EXPORTING A NEW UPDATE!
const char* currentFirmwareVersion = "1.0.3";

// 2. URL to check the latest version number
const char* versionUrl = "https://raw.githubusercontent.com/soorajds25/smartwatch_firmware/refs/heads/main/version.txt";

// 3. Base URL for downloading the .bin file
// -> The target version and filename will be appended automatically in the code.
const String firmwareBaseUrl = "https://github.com/soorajds25/smartwatch_firmware/releases/download/";
const String firmwareFilename = "/smart_watch_full_code.ino.bin";

#endif // OTA_H