#pragma once
// Icke-hemlig konfiguration. Hemligheter (WiFi-, UniFi- och admin-lösenord) matas in på
// adminsidan http://<esp>/admin och sparas i ESP:ns NVS – de finns aldrig i koden.
// För testbyggen kan include/secrets.h (gitignorerad) sätta CFG_DEFAULT_* nedan.

#define CFG_FIRMWARE_VERSION "1.2.0"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// --- WiFi (ESP:n sitter på IOT-nätet, aldrig på gästnätet). Ändras på adminsidan. ---
#ifndef CFG_DEFAULT_WIFI_SSID
#define CFG_DEFAULT_WIFI_SSID   "VidebergKraft_IOT"
#endif
#ifndef CFG_DEFAULT_WIFI_PASS
#define CFG_DEFAULT_WIFI_PASS   ""          // tomt = matas in i setup-läget
#endif
#ifndef CFG_DEFAULT_STATIC_IP
#define CFG_DEFAULT_STATIC_IP   "192.168.40.6"  // tomt = DHCP
#endif
#define CFG_HOSTNAME            "guestwifi"     // http://guestwifi.local/
#define CFG_WIFI_5GHZ_ONLY      1               // anslut bara på 5 GHz (ESP32-C5)
#define CFG_WIFI_COUNTRY        "SE"

// --- UniFi ---
#define CFG_UDM_HOST         "192.168.40.1"
#define CFG_UDM_PORT         443
#define CFG_UNIFI_SITE       "default"
#define CFG_GUEST_SSID       "VidebergKraft_Guest"

// --- Tid och schema ---
#define CFG_TZ               "CET-1CEST,M3.5.0,M10.5.0/3"   // Europe/Stockholm
#define CFG_NTP_1            "se.pool.ntp.org"
#define CFG_NTP_2            "pool.ntp.org"
#define CFG_NTP_3            "time.google.com"
#define CFG_ROTATE_HOUR      3      // nattlig rotation 03:00
#define CFG_ROTATE_MINUTE    0
#define CFG_CATCHUP_UNTIL    7      // missad rotation tas igen fram till 07:00, annars nästa natt
#define CFG_RESYNC_MINUTES   15     // läs av UDM regelbundet (fångar manuella ändringar)

// --- Kort ---
#if CONFIG_IDF_TARGET_ESP32C5
#define CFG_BOOT_BUTTON      28     // Waveshare ESP32-C5: BOOT = GPIO28
#else
#define CFG_BOOT_BUTTON      9      // ESP32-C3: BOOT = GPIO9
#endif
#ifndef CFG_HAS_OLED
#define CFG_HAS_OLED         0
#endif
#define CFG_OLED_SDA         5      // ESP32-C3 0.42" OLED-kort
#define CFG_OLED_SCL         6
