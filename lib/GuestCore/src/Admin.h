#pragma once
#include <cstdint>
#include <string>

namespace guest {

// ---------------------------------------------------------------- validering
// Returnerar "" om värdet är giltigt, annars ett felmeddelande.
std::string validateWifiPassword(const std::string& pw);    // WPA: 8–63 tecken
std::string validateAdminPassword(const std::string& pw);   // minst 10 tecken

// Normaliserar ett SHA-256-fingeravtryck till "AA:BB:...". Accepterar kolon,
// mellanslag, gemener och prefixet "sha256 Fingerprint=". Tom sträng = ogiltigt.
std::string normalizeFingerprint(const std::string& input);
std::string formatFingerprint(const uint8_t sha256[32]);

// Jämför utan att läcka var första skillnaden finns (tidsattacker).
bool constantTimeEquals(const std::string& a, const std::string& b);

// ---------------------------------------------------------------- inloggningsspärr
// Efter `maxFailures` misslyckade försök spärras inloggning i `lockMs`.
class LoginThrottle {
 public:
  LoginThrottle(int maxFailures = 5, uint32_t lockMs = 5 * 60 * 1000)
      : maxFailures_(maxFailures), lockMs_(lockMs) {}
  bool locked(uint32_t nowMs) const;
  uint32_t secondsLeft(uint32_t nowMs) const;
  void fail(uint32_t nowMs);
  void success() { failures_ = 0; }

 private:
  int maxFailures_;
  uint32_t lockMs_;
  int failures_ = 0;
  bool isLocked_ = false;
  uint32_t lockedAt_ = 0;
};

// ---------------------------------------------------------------- adminsida
// Allt sidan får veta. Avsiktligt INGA fält för hemliga värden – bara om de är satta.
struct AdminView {
  bool setupMode = false;  // första start: inget admin-lösenord ännu
  bool loggedIn = false;
  uint32_t lockedSeconds = 0;
  std::string csrf;
  std::string message;
  bool messageIsError = false;

  bool hasWifiPassword = false;
  bool hasUdmUser = false;
  bool hasUdmPassword = false;
  bool hasAdminPassword = false;
  std::string fingerprint;  // inte hemligt – visas så att det kan jämföras
  bool hasBackground = false;

  std::string wifiSsid;
  bool wifiConnected = false;
  std::string ip;
  int rssi = 0;
  std::string guestSsid;
  std::string guestPassword;  // gästlösenordet visas ändå publikt på skyltarna
  std::string lastRotated;
  std::string lastError;
  std::string time;
  std::string firmware;
};

std::string renderAdminPage(const AdminView& v);

}  // namespace guest
