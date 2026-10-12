#pragma once
#include <string>

// Hemliga inställningar i NVS. Skrivs via adminsidan och lämnar aldrig
// enheten: inget API eller någon sida returnerar dem.
struct Settings {
  std::string wifiSsid;      // inte hemligt
  std::string wifiPassword;
  std::string staticIp;      // inte hemligt; tomt = DHCP
  std::string udmUser;
  std::string udmPassword;
  std::string fingerprint;   // UDM-certifikatets SHA-256 (inte hemligt)
  std::string adminHash;     // "pbkdf2-sha256$<iter>$<salt>$<hash>" – lösenordet sparas aldrig

  bool wifiConfigured() const { return !wifiSsid.empty() && !wifiPassword.empty(); }
  bool unifiConfigured() const { return !udmUser.empty() && !udmPassword.empty(); }
};

// Värden som saknas i NVS tas från CFG_DEFAULT_* i config.h.
void settingsLoad(Settings& s);
void settingsSave(const Settings& s);
void settingsErase();

std::string hashAdminPassword(const std::string& password);
bool verifyAdminPassword(const std::string& password, const std::string& stored);

std::string randomHex(size_t bytes);
// Lättläst slumpsträng (inga 0/O/1/l/I) – för setup-nätets lösenord.
std::string randomReadable(size_t length);
