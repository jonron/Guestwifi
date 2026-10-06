#pragma once
// Icke-hemlig konfiguration. Hemligheter ligger i secrets.h (se secrets.example.h).

// --- Nätverk (ESP:n sitter på IOT-nätet, aldrig på gästnätet) ---
#define CFG_WIFI_SSID        "VidebergKraft_IOT"
#define CFG_STATIC_IP        192, 168, 40, 6
#define CFG_GATEWAY          192, 168, 40, 1
#define CFG_SUBNET           255, 255, 255, 0
#define CFG_DNS              192, 168, 40, 1

// --- UniFi ---
#define CFG_UDM_HOST         "192.168.40.1"
#define CFG_UDM_PORT         443
#define CFG_UNIFI_SITE       "default"
#define CFG_GUEST_SSID       "VidebergKraft_Guest"

// --- Tid och schema ---
#define CFG_TZ               "CET-1CEST,M3.5.0,M10.5.0/3"   // Europe/Stockholm
#define CFG_NTP_1            "se.pool.ntp.org"
#define CFG_NTP_2            "pool.ntp.org"
#define CFG_NTP_3            "192.168.40.1"
#define CFG_ROTATE_HOUR      3      // nattlig rotation 03:00
#define CFG_ROTATE_MINUTE    0
#define CFG_CATCHUP_UNTIL    7      // missad rotation tas igen fram till 07:00, annars nästa natt
#define CFG_RESYNC_MINUTES   15     // läs av UDM regelbundet (fångar manuella ändringar)

// --- OLED (ESP32-C3 0.42" 72x40 SSD1306: SDA=5, SCL=6) ---
#define CFG_OLED_SDA         5
#define CFG_OLED_SCL         6
