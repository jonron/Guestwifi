#include "Admin.h"

#include <cctype>

#include "WebContent.h"

namespace guest {

std::string validateWifiPassword(const std::string& pw) {
  if (pw.size() < 8 || pw.size() > 63) return "WiFi-lösenordet måste vara 8–63 tecken.";
  for (unsigned char c : pw)
    if (c < 0x20 || c > 0x7e) return "WiFi-lösenordet får bara innehålla vanliga ASCII-tecken.";
  return "";
}

std::string validateAdminPassword(const std::string& pw) {
  if (pw.size() < 10) return "Admin-lösenordet måste vara minst 10 tecken.";
  if (pw.size() > 128) return "Admin-lösenordet är för långt.";
  return "";
}

std::string normalizeFingerprint(const std::string& input) {
  std::string s = input;
  const size_t eq = s.find('=');
  if (eq != std::string::npos) s = s.substr(eq + 1);  // "sha256 Fingerprint=AA:BB…"
  std::string hex;
  for (unsigned char c : s) {
    if (c == ':' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (!std::isxdigit(c)) return "";
    hex += static_cast<char>(std::toupper(c));
  }
  if (hex.size() != 64) return "";
  std::string out;
  for (size_t i = 0; i < 64; i += 2) {
    if (i) out += ':';
    out += hex.substr(i, 2);
  }
  return out;
}

std::string formatFingerprint(const uint8_t sha256[32]) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (int i = 0; i < 32; ++i) {
    if (i) out += ':';
    out += digits[sha256[i] >> 4];
    out += digits[sha256[i] & 0xF];
  }
  return out;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    const unsigned char x = i < a.size() ? a[i] : 0;
    const unsigned char y = i < b.size() ? b[i] : 0;
    diff |= x ^ y;
  }
  return diff == 0;
}

bool LoginThrottle::locked(uint32_t nowMs) const {
  return isLocked_ && nowMs - lockedAt_ < lockMs_;
}

uint32_t LoginThrottle::secondsLeft(uint32_t nowMs) const {
  if (!locked(nowMs)) return 0;
  return (lockMs_ - (nowMs - lockedAt_) + 999) / 1000;
}

void LoginThrottle::fail(uint32_t nowMs) {
  if (isLocked_ && !locked(nowMs)) {
    isLocked_ = false;
    failures_ = 0;
  }
  if (++failures_ >= maxFailures_) {
    isLocked_ = true;
    lockedAt_ = nowMs;
  }
}

// ---------------------------------------------------------------- HTML
static const char* kHead = R"(<!doctype html>
<html lang="sv"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<title>Guest wifi – admin</title>
<style>
:root{--ink:#111;--muted:#666;--line:#ddd;--paper:#f6f6f3;--card:#fff;--accent:#8a2be2;--ok:#1a7f37;--err:#c62828}
@media (prefers-color-scheme:dark){:root{--ink:#eee;--muted:#aaa;--line:#333;--paper:#121212;--card:#1d1d1d;--accent:#c18cff;--ok:#5cd17a;--err:#ff7b7b}}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:15px/1.5 system-ui,sans-serif}
main{max-width:640px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:8px 0 16px}h2{font-size:16px;margin:0 0 12px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin-bottom:16px}
label{display:block;margin:12px 0 4px;font-weight:600}
.hint{color:var(--muted);font-size:13px;font-weight:400}
input[type=password],input[type=text],input[type=file]{width:100%;padding:9px 10px;border:1px solid var(--line);border-radius:6px;background:var(--paper);color:var(--ink);font:inherit}
button{margin-top:14px;padding:9px 16px;border:0;border-radius:6px;background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer}
button.secondary{background:transparent;color:var(--accent);border:1px solid var(--accent)}
button.danger{background:var(--err)}
.row{display:flex;gap:8px;flex-wrap:wrap}.row form{margin:0}
.msg{padding:10px 12px;border-radius:6px;margin-bottom:16px;border:1px solid var(--ok);color:var(--ok)}
.msg.err{border-color:var(--err);color:var(--err)}
.set{color:var(--ok)}.unset{color:var(--err)}
table{width:100%;border-collapse:collapse}td{padding:4px 0;vertical-align:top}td:first-child{color:var(--muted);width:42%}
code{font-size:12px;word-break:break-all}
</style></head><body><main>
)";

static std::string esc(const std::string& s) { return htmlEscape(s); }

static std::string state(bool set) {
  return set ? "<span class=\"set\">✓ satt</span>" : "<span class=\"unset\">✗ saknas</span>";
}

static std::string csrfField(const AdminView& v) {
  return "<input type=\"hidden\" name=\"csrf\" value=\"" + esc(v.csrf) + "\">";
}

static std::string postButton(const AdminView& v, const char* action, const char* label,
                              const char* cls = "", const char* confirm = nullptr) {
  std::string out = "<form method=\"post\" action=\"" + std::string(action) + "\"";
  if (confirm) out += " onsubmit=\"return confirm('" + std::string(confirm) + "')\"";
  out += ">" + csrfField(v) + "<button class=\"" + cls + "\">" + label + "</button></form>";
  return out;
}

static std::string secretInput(const char* name, const char* label, bool isSet, const char* hint,
                               bool required) {
  std::string out = "<label for=\"" + std::string(name) + "\">" + label + " " + state(isSet) +
                    "<br><span class=\"hint\">" + hint + "</span></label>";
  out += "<input type=\"password\" id=\"" + std::string(name) + "\" name=\"" + name +
         "\" autocomplete=\"new-password\" spellcheck=\"false\"";
  out += isSet ? " placeholder=\"lämna tomt för att behålla\"" : "";
  out += required && !isSet ? " required" : "";
  out += ">";
  return out;
}

std::string renderAdminPage(const AdminView& v) {
  std::string h = kHead;
  h += "<h1>Guest wifi – admin</h1>";
  if (!v.message.empty()) {
    h += std::string("<div class=\"msg") + (v.messageIsError ? " err" : "") + "\">" +
         esc(v.message) + "</div>";
  }

  // ---- inloggning
  if (!v.setupMode && !v.loggedIn) {
    h += "<section><h2>Logga in</h2>";
    if (v.lockedSeconds > 0) {
      h += "<p>För många felaktiga försök. Försök igen om " + std::to_string(v.lockedSeconds) +
           " sekunder.</p>";
    } else {
      h += "<form method=\"post\" action=\"/admin/login\">" + csrfField(v) +
           "<label for=\"password\">Admin-lösenord</label>"
           "<input type=\"password\" id=\"password\" name=\"password\" autocomplete=\"current-password\" required autofocus>"
           "<button>Logga in</button></form>";
    }
    h += "</section></main></body></html>";
    return h;
  }

  if (v.setupMode) {
    h += "<section><h2>Första start</h2><p>Välj ett admin-lösenord och fyll i uppgifterna nedan. "
         "Hemliga värden kan bara skrivas in – de visas aldrig igen, varken här eller via något API.</p></section>";
  }

  // ---- status
  h += "<section><h2>Status</h2><table>";
  h += "<tr><td>WiFi (" + esc(v.wifiSsid) + ")</td><td>" +
       (v.wifiConnected ? "ansluten, " + esc(v.ip) + ", RSSI " + std::to_string(v.rssi) + " dBm"
                        : std::string("inte ansluten")) + "</td></tr>";
  h += "<tr><td>Gästnät</td><td>" + esc(v.guestSsid) + "</td></tr>";
  h += "<tr><td>Gästlösenord</td><td>" + (v.guestPassword.empty() ? "–" : esc(v.guestPassword)) +
       "</td></tr>";
  h += "<tr><td>Senaste rotation</td><td>" + (v.lastRotated.empty() ? "–" : esc(v.lastRotated)) +
       "</td></tr>";
  h += "<tr><td>Senaste fel</td><td>" + (v.lastError.empty() ? "–" : esc(v.lastError)) + "</td></tr>";
  h += "<tr><td>Tid</td><td>" + (v.time.empty() ? "ej synkad" : esc(v.time)) + "</td></tr>";
  h += "<tr><td>Bakgrundsbild</td><td>" + state(v.hasBackground) + "</td></tr>";
  h += "<tr><td>Firmware</td><td>" + esc(v.firmware) + "</td></tr>";
  h += "</table>";
  if (!v.setupMode) {
    h += "<div class=\"row\">" + postButton(v, "/admin/rotate", "Rotera nu", "",
                                            "Byta gästlösenordet nu? Anslutna gäster kopplas ner.") +
         postButton(v, "/admin/test", "Testa UniFi", "secondary") + "</div>";
  }
  h += "</section>";

  // ---- hemligheter
  h += "<section><h2>Inställningar</h2><form method=\"post\" action=\"/admin/secrets\" autocomplete=\"off\">" +
       csrfField(v);
  h += secretInput("wifi_pass", ("Lösenord till " + esc(v.wifiSsid)).c_str(), v.hasWifiPassword,
                   "WPA3-lösenordet till IOT-nätet ESP:n ansluter till.", true);
  h += secretInput("udm_user", "UniFi-användare", v.hasUdmUser,
                   "Lokalt konto i UniFi OS (Site Admin, endast lokal åtkomst).", false);
  h += secretInput("udm_pass", "UniFi-lösenord", v.hasUdmPassword, "", false);
  h += "<label for=\"fingerprint\">UDM-certifikatets SHA-256 " + state(!v.fingerprint.empty()) +
       "<br><span class=\"hint\">Inte hemligt. Lämna tomt och använd knappen nedan för att hämta det från UDM:en.</span></label>"
       "<input type=\"text\" id=\"fingerprint\" name=\"fingerprint\" spellcheck=\"false\" value=\"" +
       esc(v.fingerprint) + "\">";
  h += secretInput("admin_pass", v.setupMode ? "Admin-lösenord" : "Nytt admin-lösenord",
                   v.hasAdminPassword, "Minst 10 tecken. Behövs för att komma in på den här sidan.",
                   v.setupMode);
  h += "<input type=\"password\" name=\"admin_pass2\" autocomplete=\"new-password\" placeholder=\"upprepa admin-lösenordet\"" +
       std::string(v.setupMode ? " required" : "") + " style=\"margin-top:8px\">";
  h += "<button>Spara</button><p class=\"hint\">ESP:n startar om när WiFi- eller UniFi-uppgifter ändras.</p></form>";
  if (!v.setupMode || v.hasWifiPassword) {
    h += "<div class=\"row\">" + postButton(v, "/admin/fingerprint", "Hämta fingeravtryck från UDM", "secondary") + "</div>";
  }
  h += "</section>";

  // ---- bakgrund
  h += "<section><h2>Bakgrundsbild</h2>"
       "<form method=\"post\" action=\"/admin/bg?csrf=" + esc(v.csrf) +
       "\" enctype=\"multipart/form-data\">"
       "<label for=\"bg\">JPEG, helst 1920×1080, max 800 KB</label>"
       "<input type=\"file\" id=\"bg\" name=\"bg\" accept=\"image/jpeg\" required>"
       "<button>Ladda upp</button></form></section>";

  // ---- underhåll
  h += "<section><h2>Underhåll</h2><div class=\"row\">";
  if (!v.setupMode) h += postButton(v, "/admin/logout", "Logga ut", "secondary");
  h += postButton(v, "/admin/restart", "Starta om", "secondary") +
       postButton(v, "/admin/reset", "Fabriksåterställ", "danger",
                  "Radera alla sparade lösenord och starta i setup-läge?") +
       "</div><p class=\"hint\">Fabriksåterställning går också att göra genom att hålla BOOT-knappen intryckt i 10 sekunder.</p></section>";

  h += "</main></body></html>";
  return h;
}

}  // namespace guest
