#pragma once
#include <Arduino.h>

// Loggar till både USB-porten och UART-porten, så att loggen syns oavsett
// vilken av kortets USB-C-kontakter som används.
template <typename... Args>
inline void logf(const char* fmt, Args... args) {
  Serial.printf(fmt, args...);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.printf(fmt, args...);
#endif
}

inline void logBegin() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.begin(115200);
#endif
}
