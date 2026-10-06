// Guest WiFi password rotator för ESP32-C3.
//
// Varje natt 03:00: generera "Ord-Ord-NN" -> skriv till UDM Pro Max ->
// läs tillbaka och verifiera -> först då visas lösenordet på skyltsidan.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GuestCore.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <atomic>
#include <ctime>
#include <mutex>

#include "Display.h"
#include "EspHttpTransport.h"
#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Skapa include/secrets.h utifrån include/secrets.example.h"
#endif

extern const char kIndexHtml[] asm("_binary_src_web_index_html_start");

// ------------------------------------------------------------------ tillstånd
static std::mutex gMutex;
static guest::PublicStatus gStatus;  // det som skyltarna ser (skyddas av gMutex)
static std::string gLastError;
static int gLastRotatedYmd = 0;
static int gFailures = 0;
static std::atomic<bool> gRotateRequested{false};

static Preferences gPrefs;
static WebServer gServer(80);

static EspHttpTransport gTransport(CFG_UDM_HOST, CFG_UDM_PORT, SECRET_UDM_CERT_SHA256);
static guest::UnifiClient gUnifi(gTransport, SECRET_UDM_USER, SECRET_UDM_PASS, CFG_UNIFI_SITE);
static guest::PasswordGenerator gGenerator([] { return esp_random(); });
static guest::Rotator gRotator(gUnifi, gGenerator, CFG_GUEST_SSID,
                               [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); });
static const guest::Schedule gSchedule(CFG_ROTATE_HOUR, CFG_ROTATE_MINUTE, CFG_CATCHUP_UNTIL);

// ------------------------------------------------------------------ hjälpare
static bool timeValid() { return time(nullptr) > 1700000000; }

static guest::LocalTime localNow() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  guest::LocalTime lt;
  lt.year = tm.tm_year + 1900;
  lt.month = tm.tm_mon + 1;
  lt.day = tm.tm_mday;
  lt.hour = tm.tm_hour;
  lt.minute = tm.tm_min;
  return lt;
}

static std::string isoNow() {
  if (!timeValid()) return "";
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
  return buf;
}

static void setError(const std::string& e) {
  std::lock_guard<std::mutex> lock(gMutex);
  gLastError = e;
}

// Publicera ett lösenord som routern har bekräftat.
static void publishVerified(const std::string& password, bool rotated) {
  std::lock_guard<std::mutex> lock(gMutex);
  const bool changed = gStatus.password != password;
  if (changed || !gStatus.verified) {
    gStatus.password = password;
    gStatus.verified = true;
    gStatus.updated = isoNow();
    gStatus.version++;
    gPrefs.putString("password", password.c_str());
  }
  if (rotated) gLastError.clear();
  if (changed) Serial.printf("[web] skyltsidan visar nytt lösenord (version %ld)\n", gStatus.version);
}

static void markRotated(int ymd) {
  gLastRotatedYmd = ymd;
  gPrefs.putInt("lastYmd", ymd);
}

// ------------------------------------------------------------------ rotation
static void doSync() {
  std::string current, err;
  if (!gRotator.sync(current, err)) {
    Serial.printf("[unifi] kunde inte läsa %s: %s\n", CFG_GUEST_SSID, err.c_str());
    setError(err);
    return;
  }
  publishVerified(current, false);
  if (gLastRotatedYmd == 0 && timeValid()) {
    // Första starten: adoptera det lösenord som redan gäller, rotera först i natt.
    markRotated(localNow().ymd());
    Serial.println("[rot] första start – adopterade routerns nuvarande lösenord");
  }
}

static void doRotate(bool manual) {
  std::string previous;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    previous = gStatus.password;
  }
  Serial.printf("[rot] startar %s rotation\n", manual ? "manuell" : "schemalagd");
  guest::RotateResult r = gRotator.rotate(previous);
  if (r.ok) {
    publishVerified(r.password, true);
    if (timeValid()) markRotated(localNow().ymd());
    gFailures = 0;
    Serial.println("[rot] klart – nytt lösenord verifierat i UDM");
  } else {
    gFailures++;
    setError(r.error);
    Serial.printf("[rot] misslyckades (%d): %s – gamla lösenordet gäller\n", gFailures,
                  r.error.c_str());
  }
}

static void rotationTask(void*) {
  uint32_t lastSync = 0;
  bool synced = false;
  uint32_t nextAttempt = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (WiFi.status() != WL_CONNECTED) continue;

    const uint32_t now = millis();
    if (!synced || now - lastSync > CFG_RESYNC_MINUTES * 60000UL) {
      doSync();
      synced = true;
      lastSync = millis();
    }

    if (gRotateRequested.exchange(false)) {
      doRotate(true);
      lastSync = millis();
      continue;
    }

    if (!timeValid() || (int32_t)(now - nextAttempt) < 0) continue;
    if (gSchedule.isDue(localNow(), gLastRotatedYmd)) {
      doRotate(false);
      lastSync = millis();
      if (gFailures > 0) nextAttempt = millis() + guest::Schedule::retryDelaySeconds(gFailures) * 1000;
    }
  }
}

// ------------------------------------------------------------------ webb
static void sendNoStore(int code, const char* type, const std::string& body) {
  gServer.sendHeader("Cache-Control", "no-store");
  gServer.send(code, type, body.c_str());
}

static void setupWeb() {
  gServer.on("/", HTTP_GET, [] {
    guest::PublicStatus st;
    {
      std::lock_guard<std::mutex> lock(gMutex);
      st = gStatus;
    }
    sendNoStore(200, "text/html; charset=utf-8", guest::renderPage(kIndexHtml, st));
  });

  gServer.on("/api/status", HTTP_GET, [] {
    std::string json;
    {
      std::lock_guard<std::mutex> lock(gMutex);
      json = guest::statusJson(gStatus);
    }
    sendNoStore(200, "application/json", json);
  });

  gServer.on("/api/health", HTTP_GET, [] {
    JsonDocument doc;
    {
      std::lock_guard<std::mutex> lock(gMutex);
      doc["verified"] = gStatus.verified;
      doc["last_error"] = gLastError;
    }
    doc["uptime_s"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    doc["time"] = isoNow();
    doc["last_rotated"] = gLastRotatedYmd;
    doc["failures"] = gFailures;
    std::string json;
    serializeJson(doc, json);
    sendNoStore(200, "application/json", json);
  });

  gServer.on("/api/rotate", HTTP_POST, [] {
    if (gServer.header("Authorization") != String("Bearer ") + SECRET_ADMIN_TOKEN) {
      sendNoStore(401, "text/plain", "unauthorized\n");
      return;
    }
    gRotateRequested = true;
    sendNoStore(202, "text/plain", "rotation startad\n");
  });

  gServer.serveStatic("/bg.jpg", LittleFS, "/bg.jpg", "max-age=86400");
  gServer.serveStatic("/fonts/", LittleFS, "/fonts/", "max-age=604800");
  gServer.onNotFound([] { sendNoStore(404, "text/plain", "not found\n"); });

  const char* headers[] = {"Authorization"};
  gServer.collectHeaders(headers, 1);
  gServer.begin();
}

// ------------------------------------------------------------------ WiFi
static void setupWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("guestwifi");
  WiFi.config(IPAddress(CFG_STATIC_IP), IPAddress(CFG_GATEWAY), IPAddress(CFG_SUBNET),
              IPAddress(CFG_DNS));
  WiFi.setMinSecurity(WIFI_AUTH_WPA3_PSK);  // IOT-nätet kör ren WPA3 (SAE)
  WiFi.setAutoReconnect(true);
  WiFi.begin(CFG_WIFI_SSID, SECRET_WIFI_PASSWORD);
  WiFi.setSleep(false);  // snabbare svar till skyltarna

  Serial.printf("[wifi] ansluter till %s", CFG_WIFI_SSID);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] ansluten, IP %s, RSSI %d\n", WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.println("[wifi] inte ansluten än – försöker i bakgrunden");
  }
}

// ------------------------------------------------------------------ start
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[boot] Guest WiFi password rotator");

  displayBegin();

  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS kunde inte monteras");
  if (!LittleFS.exists("/bg.jpg")) Serial.println("[fs] /bg.jpg saknas – kör 'pio run -t uploadfs'");

  gPrefs.begin("guestwifi", false);
  gLastRotatedYmd = gPrefs.getInt("lastYmd", 0);
  gStatus.ssid = CFG_GUEST_SSID;
  gStatus.password = gPrefs.getString("password", "").c_str();  // visas gråat tills routern bekräftat
  gStatus.version = 1;

  setupWifi();
  configTzTime(CFG_TZ, CFG_NTP_1, CFG_NTP_2, CFG_NTP_3);
  setupWeb();

  xTaskCreate(rotationTask, "rotation", 12288, nullptr, 1, nullptr);
}

void loop() {
  gServer.handleClient();

  // Startar om om WiFi varit nere i över 10 minuter (självläkning).
  static uint32_t downSince = 0;
  if (WiFi.status() == WL_CONNECTED) {
    downSince = 0;
  } else if (downSince == 0) {
    downSince = millis();
  } else if (millis() - downSince > 10UL * 60 * 1000) {
    Serial.println("[wifi] nere för länge – startar om");
    ESP.restart();
  }
  delay(2);
}
