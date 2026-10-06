#pragma once

// Den lilla OLED:en (72x40).
void displayBegin();
void displayNormal();  // "Guest wifi" / "pwd generator"
void displaySetup(const char* ssid, const char* password, const char* ip);
void displayMessage(const char* line1, const char* line2);
