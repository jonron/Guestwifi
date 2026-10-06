// Guest WiFi password rotator för ESP32-C3.
//
// Varje natt 03:00: generera "Ord-Ord-NN" -> skriv till UDM Pro Max ->
// läs tillbaka och verifiera -> först då visas lösenordet på skyltsidan.
//
// Hemligheter (WiFi, UniFi, admin) matas in på /admin och sparas i NVS.
// Saknas de startar ESP:n ett eget setup-nät vars namn och lösenord visas på OLED:en.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GuestCore.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>

#include "AdminWeb.h"
#include "Display.h"
#include "EspHttpTransport.h"
#include "Settings.h"
#include "config.h"

extern const char kIndexHtml[] asm("_binary_src_web_index_html_start");

// ------------------------------------------------------------------ tillstånd
static Settings gSettings;

static std::mutex gMutex;            // skyddar gStatus, gLastError
static guest::PublicStatus gStatus;  // det som skyltarna ser
static std::string gLastError;
static int gLastRotatedYmd = 0;
static int gFailures = 0;
static std::atomic<bool> gRotateRequested{false};
static std::atomic<uint32_t> gRestartAt{0};

static std::mutex gUnifiMutex;  // UniFi-klienten används av både rotationstråd och adminsida
static std::unique_ptr<EspHttpTransport> gTransport;
static std::unique_ptr<guest::UnifiClient> gUnifi;
static std::unique_ptr<guest::Rotator> gRotator;
static guest::PasswordGenerator gGenerator([] { return esp_random(); });
static const guest::Schedule gSchedule(CFG_ROTATE_HOUR, CFG_ROTATE_MINUTE, CFG_CATCHUP_UNTIL);

static Preferences gPrefs;
static WebServer gServer(80);

static bool gApActive = false;
static uint32_t gApStartedAt = 0;
static std::string gApSsid;

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
  bool ok;
  {
    std::lock_guard<std::mutex> lock(gUnifiMutex);
    ok = gRotator->sync(current, err);
  }
  if (!ok) {
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
  guest::RotateResult r;
  {
    std::lock_guard<std::mutex> lock(gUnifiMutex);
    r = gRotator->rotate(previous);
  }
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
    if (WiFi.status() != WL_CONNECTED || gSettings.udmUser.empty()) continue;

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

// ------------------------------------------------------------------ setup-nät
// Eget WiFi-nät för första inställningen (eller när IOT-nätet inte går att nå).
static void startSetupAp() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[24];
  snprintf(ssid, sizeof(ssid), "GW-Setup-%02X%02X", mac[4], mac[5]);
  gApSsid = ssid;
  const std::string pass = randomReadable(10);

  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, pass.c_str());
  gApActive = true;
  gApStartedAt = millis();

  const std::string ip = WiFi.softAPIP().toString().c_str();
  displaySetup(ssid, pass.c_str(), ip.c_str());
  Serial.printf("[setup] eget nät %s – lösenordet visas på skärmen. Öppna http://%s/admin\n", ssid,
                ip.c_str());
}

// ------------------------------------------------------------------ adminsidans koppling
static void fillAdminStatus(guest::AdminView& v) {
  v.wifiSsid = CFG_WIFI_SSID;
  v.wifiConnected = WiFi.status() == WL_CONNECTED;
  v.ip = WiFi.localIP().toString().c_str();
  v.rssi = WiFi.RSSI();
  v.guestSsid = CFG_GUEST_SSID;
  v.time = isoNow();
  v.firmware = CFG_FIRMWARE_VERSION;
  if (gLastRotatedYmd) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", gLastRotatedYmd / 10000, gLastRotatedYmd / 100 % 100,
             gLastRotatedYmd % 100);
    v.lastRotated = buf;
  }
  std::lock_guard<std::mutex> lock(gMutex);
  v.guestPassword = gStatus.verified ? gStatus.password : "";
  v.lastError = gLastError;
}

static bool testUnifi(std::string& message) {
  if (!gUnifi) {
    message = "Inte ansluten till IOT-nätet.";
    return false;
  }
  if (gSettings.udmUser.empty()) {
    message = "Fyll i UniFi-användare och lösenord först.";
    return false;
  }
  std::unique_lock<std::mutex> lock(gUnifiMutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    message = "En rotation pågår – försök igen om en minut.";
    return false;
  }
  std::string pw;
  if (!gUnifi->readPassphrase(CFG_GUEST_SSID, pw)) {
    message = "UniFi-test misslyckades: " + gUnifi->lastError();
    return false;
  }
  message = std::string("UniFi fungerar – hittade ") + CFG_GUEST_SSID + ".";
  return true;
}

static bool fetchFingerprint(std::string& fingerprint, std::string& error) {
  if (WiFi.status() != WL_CONNECTED) {
    error = "inte ansluten till IOT-nätet";
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(CFG_UDM_HOST, CFG_UDM_PORT, 10000)) {
    error = std::string("kan inte ansluta till ") + CFG_UDM_HOST;
    return false;
  }
  uint8_t sha[32];
  const bool ok = client.getFingerprintSHA256(sha);
  client.stop();
  if (!ok) {
    error = "fick inget certifikat";
    return false;
  }
  fingerprint = guest::formatFingerprint(sha);
  return true;
}

// ------------------------------------------------------------------ webb
static void sendNoStore(int code, const char* type, const std::string& body) {
  gServer.sendHeader("Cache-Control", "no-store");
  gServer.send(code, type, body.c_str());
}

static void setupWeb() {
  gServer.on("/", HTTP_GET, [] {
    if (!gSettings.provisioned()) {
      gServer.sendHeader("Location", "/admin");
      gServer.send(303, "text/plain", "");
      return;
    }
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
    doc["firmware"] = CFG_FIRMWARE_VERSION;
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

  AdminContext ctx;
  ctx.settings = &gSettings;
  ctx.fillStatus = fillAdminStatus;
  ctx.testUnifi = testUnifi;
  ctx.fetchFingerprint = fetchFingerprint;
  ctx.requestRotate = [] { gRotateRequested = true; };
  ctx.scheduleRestart = [](uint32_t delayMs) { gRestartAt = millis() + delayMs; };
  adminBegin(gServer, ctx);

  gServer.serveStatic("/bg.jpg", LittleFS, "/bg.jpg", "max-age=3600");
  gServer.serveStatic("/fonts/", LittleFS, "/fonts/", "max-age=604800");
  gServer.onNotFound([] { sendNoStore(404, "text/plain", "not found\n"); });

  const char* headers[] = {"Cookie"};
  gServer.collectHeaders(headers, 1);
  gServer.begin();
}

// ------------------------------------------------------------------ WiFi
static void startStation() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("guestwifi");
  WiFi.config(IPAddress(CFG_STATIC_IP), IPAddress(CFG_GATEWAY), IPAddress(CFG_SUBNET),
              IPAddress(CFG_DNS));
  WiFi.setMinSecurity(WIFI_AUTH_WPA3_PSK);  // IOT-nätet kör ren WPA3 (SAE)
  WiFi.setAutoReconnect(true);
  WiFi.begin(CFG_WIFI_SSID, gSettings.wifiPassword.c_str());
  WiFi.setSleep(false);  // snabbare svar till skyltarna

  Serial.printf("[wifi] ansluter till %s", CFG_WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) {
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

// ------------------------------------------------------------------ BOOT-knappen
// Håll BOOT intryckt i 10 s för att radera alla sparade lösenord (fysisk åtkomst krävs).
static void checkFactoryResetButton() {
  static uint32_t pressedSince = 0;
  static bool shown = false;
  if (digitalRead(CFG_BOOT_BUTTON) == LOW) {
    if (pressedSince == 0) pressedSince = millis();
    const uint32_t held = millis() - pressedSince;
    if (held > 3000 && !shown) {
      displayMessage("Reset?", "hall kvar 10 s");
      shown = true;
    }
    if (held > 10000) {
      settingsErase();
      displayMessage("Reset", "startar om");
      Serial.println("[setup] fabriksåterställd med BOOT-knappen");
      delay(1000);
      ESP.restart();
    }
  } else if (pressedSince != 0) {
    pressedSince = 0;
    if (shown) {
      shown = false;
      if (!gApActive) displayNormal();
    }
  }
}

// ------------------------------------------------------------------ start
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[boot] Guest WiFi password rotator %s\n", CFG_FIRMWARE_VERSION);

  pinMode(CFG_BOOT_BUTTON, INPUT_PULLUP);
  displayBegin();

  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS kunde inte monteras");

  settingsLoad(gSettings);
  gPrefs.begin("guestwifi", false);
  gLastRotatedYmd = gPrefs.getInt("lastYmd", 0);
  gStatus.ssid = CFG_GUEST_SSID;
  gStatus.password = gPrefs.getString("password", "").c_str();  // gråat tills routern bekräftat
  gStatus.version = 1;

  if (!gSettings.provisioned()) {
    Serial.println("[setup] inga inställningar sparade – startar setup-läge");
    startSetupAp();
    setupWeb();
    return;
  }

  gTransport.reset(new EspHttpTransport(CFG_UDM_HOST, CFG_UDM_PORT, gSettings.fingerprint));
  gUnifi.reset(new guest::UnifiClient(*gTransport, gSettings.udmUser, gSettings.udmPassword,
                                      CFG_UNIFI_SITE));
  gRotator.reset(new guest::Rotator(*gUnifi, gGenerator, CFG_GUEST_SSID,
                                    [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }));
  if (gSettings.fingerprint.empty()) {
    Serial.println("[unifi] VARNING: inget certifikat-fingeravtryck – UDM:ens identitet kontrolleras inte");
  }

  startStation();
  configTzTime(CFG_TZ, CFG_NTP_1, CFG_NTP_2, CFG_NTP_3);
  setupWeb();
  xTaskCreate(rotationTask, "rotation", 12288, nullptr, 1, nullptr);
}

void loop() {
  gServer.handleClient();
  checkFactoryResetButton();

  const uint32_t restartAt = gRestartAt;
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    Serial.println("[boot] startar om");
    delay(200);
    ESP.restart();
  }

  if (!gSettings.provisioned()) {
    delay(2);
    return;  // setup-läge: bara webbservern
  }

  // Når vi inte IOT-nätet på 3 minuter startas setup-nätet så att lösenordet kan rättas.
  // Efter 15 minuter utan inloggad admin startar ESP:n om och försöker igen.
  static uint32_t downSince = 0;
  if (WiFi.status() == WL_CONNECTED) {
    downSince = 0;
  } else if (!gApActive) {
    if (downSince == 0) downSince = millis();
    if (millis() - downSince > 3UL * 60 * 1000) {
      Serial.println("[wifi] når inte IOT-nätet – startar setup-nätet");
      startSetupAp();
    }
  } else if (millis() - gApStartedAt > 15UL * 60 * 1000 && !adminRecentlyActive(5UL * 60 * 1000)) {
    Serial.println("[wifi] setup-nätet oanvänt – startar om och försöker igen");
    ESP.restart();
  }
  delay(2);
}
