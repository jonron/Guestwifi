// Guest WiFi password rotator (ESP32-C5 / ESP32-C3).
//
// Varje natt 03:00: generera "ord_ord_NN" -> skriv till UDM Pro Max ->
// läs tillbaka och verifiera -> först då visas lösenordet på skyltsidan.
// Utan UniFi-konto körs ett lokalt testläge där lösenordet bara byts i ESP:n.
//
// Hemligheter (WiFi, UniFi, admin) matas in på /admin och sparas i NVS.
// Saknas WiFi-uppgifter startar ESP:n ett eget setup-nät (lösenordet skrivs i
// seriell-loggen och visas på OLED:en om kortet har en).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <GuestCore.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>

#include "AdminWeb.h"
#include "Display.h"
#include "EspHttpTransport.h"
#include "Log.h"
#include "Settings.h"
#include "config.h"

#if CFG_WIFI_5GHZ_ONLY && defined(CONFIG_SOC_WIFI_SUPPORT_5G) && CONFIG_SOC_WIFI_SUPPORT_5G
#define WIFI_5GHZ_ONLY 1
#else
#define WIFI_5GHZ_ONLY 0  // kortet saknar 5 GHz (t.ex. ESP32-C3)
#endif

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

static std::mutex gWlanMutex;  // WLAN-backenden används av både rotationstråd och adminsida
static std::unique_ptr<EspHttpTransport> gTransport;
static std::unique_ptr<guest::WlanApi> gWlan;  // UnifiClient eller LocalWlanApi
static std::unique_ptr<guest::Rotator> gRotator;
static guest::PasswordGenerator gGenerator([] { return esp_random(); });
static const guest::Schedule gSchedule(CFG_ROTATE_HOUR, CFG_ROTATE_MINUTE, CFG_CATCHUP_UNTIL);

static Preferences gPrefs;
static WebServer gServer(80);

static bool gApActive = false;
static uint32_t gApStartedAt = 0;
static std::string gApSsid, gApPass;

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
  lt.second = tm.tm_sec;
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

static std::string currentAddress() {
  if (gSettings.staticIp.empty()) return std::string(CFG_HOSTNAME) + ".local";
  return gSettings.staticIp;
}

// Publicera ett lösenord som routern (eller lokala testläget) har bekräftat.
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
  if (changed) logf("[web] skyltsidan visar nytt lösenord (version %ld)\n", gStatus.version);
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
    std::lock_guard<std::mutex> lock(gWlanMutex);
    ok = gRotator->sync(current, err);
  }
  if (!ok) {
    logf("[wlan] kunde inte läsa %s: %s\n", CFG_GUEST_SSID, err.c_str());
    setError(err);
    statusLed(LedState::Error);
    return;
  }
  publishVerified(current, false);
  if (gLastRotatedYmd == 0 && timeValid()) {
    // Första starten: adoptera det lösenord som redan gäller, rotera först i natt.
    markRotated(localNow().ymd());
    logf("[rot] första start – behåller nuvarande lösenord, nästa byte i natt\n");
  }
}

static void doRotate(bool manual) {
  std::string previous;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    previous = gStatus.password;
  }
  logf("[rot] startar %s rotation\n", manual ? "manuell" : "schemalagd");
  statusLed(LedState::Rotating);
  guest::RotateResult r;
  {
    std::lock_guard<std::mutex> lock(gWlanMutex);
    r = gRotator->rotate(previous);
  }
  if (r.ok) {
    publishVerified(r.password, true);
    if (timeValid()) markRotated(localNow().ymd());
    gFailures = 0;
    statusLed(LedState::Online);
    logf("[rot] klart – nytt lösenord verifierat\n");
  } else {
    gFailures++;
    setError(r.error);
    statusLed(LedState::Error);
    logf("[rot] misslyckades (%d): %s – gamla lösenordet gäller\n", gFailures, r.error.c_str());
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

// ------------------------------------------------------------------ WiFi
static void applyRadioPolicy() {
  esp_wifi_set_country_code(CFG_WIFI_COUNTRY, true);
#if WIFI_5GHZ_ONLY
  if (!WiFi.setBandMode(WIFI_BAND_MODE_5G_ONLY)) logf("[wifi] kunde inte låsa till 5 GHz\n");
#endif
}

// Eget WiFi-nät för första inställningen (eller när ordinarie nät inte går att nå).
static void startSetupAp() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[24];
  snprintf(ssid, sizeof(ssid), "GW-Setup-%02X%02X", mac[4], mac[5]);
  gApSsid = ssid;
  gApPass = randomReadable(10);

  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  applyRadioPolicy();
#if WIFI_5GHZ_ONLY
  WiFi.softAP(ssid, gApPass.c_str(), 36);  // kanal 36 (5 GHz, utan DFS)
#else
  WiFi.softAP(ssid, gApPass.c_str());
#endif
  gApActive = true;
  gApStartedAt = millis();

  const std::string ip = WiFi.softAPIP().toString().c_str();
  displaySetup(ssid, gApPass.c_str(), ip.c_str());
  statusLed(LedState::Setup);
  logf("[setup] eget nät %s, lösenord %s – öppna http://%s/admin\n", ssid, gApPass.c_str(),
       ip.c_str());
}

static bool parseIp(const std::string& s, IPAddress& out) { return out.fromString(s.c_str()); }

static void startStation() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  applyRadioPolicy();
  WiFi.setHostname(CFG_HOSTNAME);

  IPAddress ip;
  if (!gSettings.staticIp.empty() && parseIp(gSettings.staticIp, ip)) {
    const IPAddress gw(ip[0], ip[1], ip[2], 1);
    WiFi.config(ip, gw, IPAddress(255, 255, 255, 0), gw);
  }
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);  // WPA2 eller WPA3
  WiFi.setAutoReconnect(true);
  statusLed(LedState::Connecting);
  WiFi.begin(gSettings.wifiSsid.c_str(), gSettings.wifiPassword.c_str());
  WiFi.setSleep(false);  // snabbare svar till skyltarna

  logf("[wifi] ansluter till %s%s", gSettings.wifiSsid.c_str(),
       WIFI_5GHZ_ONLY ? " (endast 5 GHz)" : "");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(500);
    logf(".");
  }
  logf("\n");
}

// Logga anslutning och starta mDNS när WiFi kommer upp (även efter avbrott).
static void onWifiEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    logf("[wifi] ansluten: IP %s, kanal %d (%s), RSSI %d dBm\n",
         WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.channel() > 14 ? "5 GHz" : "2,4 GHz",
         WiFi.RSSI());
    MDNS.end();
    if (MDNS.begin(CFG_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    logf("[web] skyltsida: http://%s/  admin: http://%s/admin  (även http://%s.local/)\n",
         WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str(), CFG_HOSTNAME);
    statusLed(LedState::Online);
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && !gApActive) {
    statusLed(LedState::Connecting);
  }
}

// ------------------------------------------------------------------ adminsidans koppling
static void fillAdminStatus(guest::AdminView& v) {
  v.wifiConnected = WiFi.status() == WL_CONNECTED;
  v.ip = WiFi.localIP().toString().c_str();
  v.rssi = WiFi.RSSI();
  if (v.wifiConnected) {
    v.band = std::string(WiFi.channel() > 14 ? "5 GHz" : "2,4 GHz") + ", kanal " +
             std::to_string(WiFi.channel());
  }
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
  if (!gSettings.unifiConfigured() || !gWlan) {
    message = "Fyll i UniFi-användare och lösenord först.";
    return false;
  }
  std::unique_lock<std::mutex> lock(gWlanMutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    message = "En rotation pågår – försök igen om en minut.";
    return false;
  }
  std::string pw;
  if (!gWlan->readPassphrase(CFG_GUEST_SSID, pw)) {
    message = "UniFi-test misslyckades: " + gWlan->lastError();
    return false;
  }
  message = std::string("UniFi fungerar – hittade ") + CFG_GUEST_SSID + ".";
  return true;
}

static bool fetchFingerprint(std::string& fingerprint, std::string& error) {
  if (WiFi.status() != WL_CONNECTED) {
    error = "inte ansluten till WiFi";
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
    if (!gSettings.wifiConfigured()) {
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

  // För e-pappersskärmen (reTerminal E1002): lösenord + när den ska vakna nästa gång.
  gServer.on("/api/display", HTTP_GET, [] {
    const long next = timeValid() ? gSchedule.secondsUntilNextRotation(localNow(), gLastRotatedYmd) : -1;
    std::string json;
    {
      std::lock_guard<std::mutex> lock(gMutex);
      json = guest::displayJson(gStatus, next);
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
    doc["mode"] = gSettings.unifiConfigured() ? "unifi" : "local-test";
    doc["uptime_s"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    doc["channel"] = WiFi.channel();
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

// ------------------------------------------------------------------ BOOT-knappen
// Håll BOOT intryckt i 10 s för att radera alla sparade lösenord (fysisk åtkomst krävs).
static void checkFactoryResetButton() {
  static uint32_t pressedSince = 0;
  static bool warned = false;
  if (digitalRead(CFG_BOOT_BUTTON) == LOW) {
    if (pressedSince == 0) pressedSince = millis();
    const uint32_t held = millis() - pressedSince;
    if (held > 3000 && !warned) {
      displayMessage("Reset?", "hall kvar 10 s");
      statusLed(LedState::Error);
      logf("[setup] håll BOOT i 10 s för fabriksåterställning\n");
      warned = true;
    }
    if (held > 10000) {
      settingsErase();
      displayMessage("Reset", "startar om");
      logf("[setup] fabriksåterställd med BOOT-knappen\n");
      delay(1000);
      ESP.restart();
    }
  } else if (pressedSince != 0) {
    pressedSince = 0;
    if (warned) {
      warned = false;
      if (!gApActive) displayNormal();
      statusLed(gApActive ? LedState::Setup
                          : WiFi.status() == WL_CONNECTED ? LedState::Online : LedState::Connecting);
    }
  }
}

// ------------------------------------------------------------------ start
void setup() {
  logBegin();
  delay(300);
  logf("\n[boot] Guest WiFi password rotator %s\n", CFG_FIRMWARE_VERSION);

  pinMode(CFG_BOOT_BUTTON, INPUT_PULLUP);
  displayBegin();
  statusLed(LedState::Booting);

  if (!LittleFS.begin(true)) logf("[fs] LittleFS kunde inte monteras\n");

  settingsLoad(gSettings);
  gPrefs.begin("guestwifi", false);
  gLastRotatedYmd = gPrefs.getInt("lastYmd", 0);
  gStatus.ssid = CFG_GUEST_SSID;
  gStatus.password = gPrefs.getString("password", "").c_str();  // gråat tills det bekräftats
  gStatus.version = 1;

  WiFi.onEvent(onWifiEvent);

  if (!gSettings.wifiConfigured()) {
    logf("[setup] inga WiFi-uppgifter sparade – startar setup-läge\n");
    startSetupAp();
    setupWeb();
    return;
  }

  if (gSettings.unifiConfigured()) {
    gTransport.reset(new EspHttpTransport(CFG_UDM_HOST, CFG_UDM_PORT, gSettings.fingerprint));
    gWlan.reset(new guest::UnifiClient(*gTransport, gSettings.udmUser, gSettings.udmPassword,
                                       CFG_UNIFI_SITE));
    gRotator.reset(new guest::Rotator(*gWlan, gGenerator, CFG_GUEST_SSID,
                                      [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }));
    if (gSettings.fingerprint.empty()) {
      logf("[unifi] VARNING: inget certifikat-fingeravtryck – UDM:ens identitet kontrolleras inte\n");
    }
  } else {
    // Lokalt testläge: inget UniFi-konto. Första gången skapas ett lösenord direkt.
    std::string initial = gStatus.password.empty() ? gGenerator.generate() : gStatus.password;
    gWlan.reset(new guest::LocalWlanApi(initial));
    gRotator.reset(new guest::Rotator(*gWlan, gGenerator, CFG_GUEST_SSID,
                                      [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }, 3, 300));
    publishVerified(initial, false);
    logf("[rot] lokalt testläge (inget UniFi-konto) – lösenordet byts bara i ESP:n\n");
  }
  if (gSettings.adminHash.empty()) {
    logf("[admin] inget admin-lösenord ännu – välj ett på /admin\n");
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
    logf("[boot] startar om\n");
    delay(200);
    ESP.restart();
  }

  if (gApActive) {
    // Upprepa setup-uppgifterna i loggen, så att de syns även om konsolen öppnas senare.
    static uint32_t lastHint = 0;
    if (millis() - lastHint > 15000) {
      lastHint = millis();
      logf("[setup] nät %s, lösenord %s, http://%s/admin\n", gApSsid.c_str(), gApPass.c_str(),
           WiFi.softAPIP().toString().c_str());
    }
  }

  if (!gSettings.wifiConfigured()) {
    delay(2);
    return;  // setup-läge: bara webbservern
  }

  // Når vi inte WiFi på 3 minuter startas setup-nätet så att uppgifterna kan rättas.
  // Efter 15 minuter utan inloggad admin startar ESP:n om och försöker igen.
  static uint32_t downSince = 0;
  if (WiFi.status() == WL_CONNECTED) {
    downSince = 0;
  } else if (!gApActive) {
    if (downSince == 0) downSince = millis();
    if (millis() - downSince > 3UL * 60 * 1000) {
      logf("[wifi] når inte %s – startar setup-nätet\n", gSettings.wifiSsid.c_str());
      startSetupAp();
    }
  } else if (millis() - gApStartedAt > 15UL * 60 * 1000 && !adminRecentlyActive(5UL * 60 * 1000)) {
    logf("[wifi] setup-nätet oanvänt – startar om och försöker igen\n");
    ESP.restart();
  }
  delay(2);
}
