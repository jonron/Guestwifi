#pragma once

// Statusvisning: OLED (ESP32-C3-kortet) och/eller RGB-lysdiod (ESP32-C5-kortet).
enum class LedState { Booting, Connecting, Online, Setup, Error, Rotating };

void displayBegin();
void displayNormal();  // "Guest wifi" / "pwd generator"
void displaySetup(const char* ssid, const char* password, const char* ip);
void displayMessage(const char* line1, const char* line2);
void statusLed(LedState state);
