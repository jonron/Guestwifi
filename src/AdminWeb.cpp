#include "AdminWeb.h"

#include <LittleFS.h>
#include <WiFi.h>

#include <utility>

static constexpr uint32_t kSessionTimeoutMs = 15 * 60 * 1000;
static constexpr size_t kMaxBackgroundBytes = 800 * 1024;
static constexpr const char* kCookieName = "gw_s";

static WebServer* sServer = nullptr;
static AdminContext sCtx;

// En session åt gången räcker för en adminsida.
static std::string sSession;
static std::string sSessionCsrf;
static uint32_t sSessionLast = 0;
static std::string sAnonCsrf;  // för inloggnings- och setup-formulär
static guest::LoginThrottle sThrottle;

static std::string sFlash;
static bool sFlashError = false;

static File sUpload;
static size_t sUploadSize = 0;
static std::string sUploadError;

// ---------------------------------------------------------------- hjälpare
static bool setupMode() { return sCtx.settings->adminHash.empty(); }

static std::string cookieValue(const String& header, const char* name) {
  const std::string h = header.c_str();
  const std::string key = std::string(name) + "=";
  size_t pos = 0;
  while ((pos = h.find(key, pos)) != std::string::npos) {
    if (pos == 0 || h[pos - 1] == ' ' || h[pos - 1] == ';') {
      const size_t start = pos + key.size();
      const size_t end = h.find(';', start);
      return h.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    pos += key.size();
  }
  return "";
}

static bool loggedIn() {
  if (sSession.empty()) return false;
  if (millis() - sSessionLast > kSessionTimeoutMs) {
    sSession.clear();
    return false;
  }
  if (!guest::constantTimeEquals(cookieValue(sServer->header("Cookie"), kCookieName), sSession)) {
    return false;
  }
  sSessionLast = millis();
  return true;
}

static bool authorized() { return setupMode() || loggedIn(); }

static const std::string& currentCsrf() { return loggedIn() ? sSessionCsrf : sAnonCsrf; }

static bool csrfValid() {
  return guest::constantTimeEquals(sServer->arg("csrf").c_str(), currentCsrf());
}

static void flash(const std::string& message, bool isError = false) {
  sFlash = message;
  sFlashError = isError;
}

static void redirectToAdmin() {
  sServer->sendHeader("Location", "/admin");
  sServer->sendHeader("Cache-Control", "no-store");
  sServer->send(303, "text/plain", "");
}

// Gemensam kontroll för alla formulär som kräver behörighet.
static bool guard() {
  if (!authorized()) {
    flash("Logga in först.", true);
    redirectToAdmin();
    return false;
  }
  if (!csrfValid()) {
    flash("Formuläret har gått ut – försök igen.", true);
    redirectToAdmin();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- sidor
static void handlePage() {
  guest::AdminView v;
  v.setupMode = setupMode();
  v.loggedIn = loggedIn();
  v.lockedSeconds = sThrottle.secondsLeft(millis());
  v.csrf = currentCsrf();
  v.message = sFlash;
  v.messageIsError = sFlashError;
  sFlash.clear();

  const Settings& s = *sCtx.settings;
  v.hasWifiPassword = !s.wifiPassword.empty();
  v.hasUdmUser = !s.udmUser.empty();
  v.hasUdmPassword = !s.udmPassword.empty();
  v.hasAdminPassword = !s.adminHash.empty();
  v.fingerprint = s.fingerprint;
  v.hasBackground = LittleFS.exists("/bg.jpg");
  if (v.setupMode || v.loggedIn) sCtx.fillStatus(v);

  sServer->sendHeader("Cache-Control", "no-store");
  sServer->sendHeader("X-Frame-Options", "DENY");
  sServer->sendHeader("Content-Security-Policy",
                      "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
                      "frame-ancestors 'none'; script-src 'unsafe-inline'");
  sServer->send(200, "text/html; charset=utf-8", guest::renderAdminPage(v).c_str());
}

static void handleLogin() {
  if (setupMode()) return redirectToAdmin();
  const uint32_t now = millis();
  if (sThrottle.locked(now)) return redirectToAdmin();
  if (!guest::constantTimeEquals(sServer->arg("csrf").c_str(), sAnonCsrf)) {
    flash("Formuläret har gått ut – försök igen.", true);
    return redirectToAdmin();
  }
  if (!verifyAdminPassword(sServer->arg("password").c_str(), sCtx.settings->adminHash)) {
    sThrottle.fail(now);
    Serial.println("[admin] misslyckad inloggning");
    flash("Fel lösenord.", true);
    return redirectToAdmin();
  }
  sThrottle.success();
  sSession = randomHex(16);
  sSessionCsrf = randomHex(16);
  sSessionLast = millis();
  sServer->sendHeader("Set-Cookie", String(kCookieName) + "=" + sSession.c_str() +
                                        "; Path=/admin; HttpOnly; SameSite=Strict");
  Serial.println("[admin] inloggad");
  redirectToAdmin();
}

static void handleLogout() {
  if (!guard()) return;
  sSession.clear();
  sServer->sendHeader("Set-Cookie", String(kCookieName) + "=; Path=/admin; Max-Age=0");
  flash("Utloggad.");
  redirectToAdmin();
}

static void handleSecrets() {
  if (!guard()) return;
  Settings next = *sCtx.settings;
  const bool firstSetup = setupMode();

  const std::string wifi = sServer->arg("wifi_pass").c_str();
  const std::string udmUser = sServer->arg("udm_user").c_str();
  const std::string udmPass = sServer->arg("udm_pass").c_str();
  const std::string fp = sServer->arg("fingerprint").c_str();
  const std::string admin = sServer->arg("admin_pass").c_str();
  const std::string admin2 = sServer->arg("admin_pass2").c_str();

  std::string err;
  if (!wifi.empty()) {
    err = guest::validateWifiPassword(wifi);
    next.wifiPassword = wifi;
  } else if (next.wifiPassword.empty()) {
    err = "WiFi-lösenordet behövs.";
  }
  if (err.empty() && !fp.empty()) {
    next.fingerprint = guest::normalizeFingerprint(fp);
    if (next.fingerprint.empty()) err = "Fingeravtrycket ska vara 64 hextecken (SHA-256).";
  }
  if (err.empty() && (!admin.empty() || firstSetup)) {
    err = guest::validateAdminPassword(admin);
    if (err.empty() && admin != admin2) err = "Admin-lösenorden är inte lika.";
    if (err.empty()) next.adminHash = hashAdminPassword(admin);
  }
  if (!err.empty()) {
    flash(err, true);
    return redirectToAdmin();
  }
  if (!udmUser.empty()) next.udmUser = udmUser;
  if (!udmPass.empty()) next.udmPassword = udmPass;

  const bool needsRestart = firstSetup || next.wifiPassword != sCtx.settings->wifiPassword ||
                            next.udmUser != sCtx.settings->udmUser ||
                            next.udmPassword != sCtx.settings->udmPassword ||
                            next.fingerprint != sCtx.settings->fingerprint;
  *sCtx.settings = next;
  settingsSave(next);
  Serial.println("[admin] inställningar sparade");  // aldrig värdena

  if (needsRestart) {
    flash(firstSetup ? "Sparat. ESP:n startar om och ansluter till IOT-nätet – öppna sedan "
                       "http://192.168.40.6/admin"
                     : "Sparat. ESP:n startar om…");
    sCtx.scheduleRestart(1500);
  } else {
    flash("Sparat.");
  }
  redirectToAdmin();
}

static void handleFingerprint() {
  if (!guard()) return;
  std::string fp, err;
  if (!sCtx.fetchFingerprint(fp, err)) {
    flash("Kunde inte hämta fingeravtrycket: " + err, true);
    return redirectToAdmin();
  }
  const bool changed = fp != sCtx.settings->fingerprint;
  sCtx.settings->fingerprint = fp;
  settingsSave(*sCtx.settings);
  flash("Fingeravtrycket hämtat och sparat: " + fp +
        (changed ? ". Jämför gärna med UniFi OS. ESP:n startar om…" : ""));
  if (changed) sCtx.scheduleRestart(1500);
  redirectToAdmin();
}

static void handleTest() {
  if (!guard()) return;
  std::string message;
  const bool ok = sCtx.testUnifi(message);
  flash(message, !ok);
  redirectToAdmin();
}

static void handleRotate() {
  if (!guard()) return;
  sCtx.requestRotate();
  flash("Rotation startad. Ladda om sidan om en minut för att se resultatet.");
  redirectToAdmin();
}

static void handleRestart() {
  if (!guard()) return;
  flash("Startar om…");
  sCtx.scheduleRestart(1000);
  redirectToAdmin();
}

static void handleReset() {
  if (!guard()) return;
  settingsErase();
  *sCtx.settings = Settings();
  Serial.println("[admin] fabriksåterställd via adminsidan");
  flash("Alla sparade lösenord är raderade. ESP:n startar om i setup-läge.");
  sCtx.scheduleRestart(1500);
  redirectToAdmin();
}

// Uppladdning av bakgrundsbild – tas emot i bitar och skrivs till LittleFS.
static void handleUploadChunk() {
  HTTPUpload& up = sServer->upload();
  if (up.status == UPLOAD_FILE_START) {
    sUploadSize = 0;
    sUploadError.clear();
    if (!authorized() || !csrfValid()) {
      sUploadError = "Logga in först.";
      return;
    }
    sUpload = LittleFS.open("/bg.tmp", "w");
    if (!sUpload) sUploadError = "Kunde inte skriva till filsystemet.";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!sUploadError.empty() || !sUpload) return;
    if (sUploadSize == 0 && (up.currentSize < 2 || up.buf[0] != 0xFF || up.buf[1] != 0xD8)) {
      sUploadError = "Filen är inte en JPEG-bild.";
    } else if (sUploadSize + up.currentSize > kMaxBackgroundBytes) {
      sUploadError = "Bilden är större än 800 KB.";
    } else if (sUpload.write(up.buf, up.currentSize) != up.currentSize) {
      sUploadError = "Filsystemet är fullt.";
    }
    sUploadSize += up.currentSize;
    if (!sUploadError.empty()) sUpload.close();
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (sUpload) sUpload.close();
    if (up.status == UPLOAD_FILE_ABORTED) sUploadError = "Uppladdningen avbröts.";
    if (sUploadError.empty()) {
      LittleFS.remove("/bg.jpg");
      LittleFS.rename("/bg.tmp", "/bg.jpg");
    } else {
      LittleFS.remove("/bg.tmp");
    }
  }
}

static void handleUploadDone() {
  if (sUploadError.empty() && sUploadSize > 0) {
    flash("Bakgrundsbilden är uppladdad (" + std::to_string(sUploadSize / 1024) +
          " KB). Skyltarna visar den vid nästa omladdning.");
  } else {
    flash(sUploadError.empty() ? "Ingen fil mottogs." : sUploadError, true);
  }
  redirectToAdmin();
}

// ---------------------------------------------------------------- registrering
void adminBegin(WebServer& server, AdminContext ctx) {
  sServer = &server;
  sCtx = std::move(ctx);
  sAnonCsrf = randomHex(16);

  server.on("/admin", HTTP_GET, handlePage);
  server.on("/admin/login", HTTP_POST, handleLogin);
  server.on("/admin/logout", HTTP_POST, handleLogout);
  server.on("/admin/secrets", HTTP_POST, handleSecrets);
  server.on("/admin/fingerprint", HTTP_POST, handleFingerprint);
  server.on("/admin/test", HTTP_POST, handleTest);
  server.on("/admin/rotate", HTTP_POST, handleRotate);
  server.on("/admin/restart", HTTP_POST, handleRestart);
  server.on("/admin/reset", HTTP_POST, handleReset);
  server.on("/admin/bg", HTTP_POST, handleUploadDone, handleUploadChunk);
}

bool adminRecentlyActive(uint32_t withinMs) {
  return !sSession.empty() && millis() - sSessionLast < withinMs;
}
