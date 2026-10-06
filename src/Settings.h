#pragma once
#include <string>

// Hemliga inställningar i NVS. Skrivs via adminsidan och lämnar aldrig
// enheten: inget API eller någon sida returnerar dem.
struct Settings {
  std::string wifiPassword;  // till CFG_WIFI_SSID
  std::string udmUser;
  std::string udmPassword;
  std::string fingerprint;   // UDM-certifikatets SHA-256 (inte hemligt)
  std::string adminHash;     // "pbkdf2-sha256$<iter>$<salt>$<hash>" – lösenordet sparas aldrig

  bool provisioned() const { return !wifiPassword.empty() && !adminHash.empty(); }
};

void settingsLoad(Settings& s);
void settingsSave(const Settings& s);
void settingsErase();

std::string hashAdminPassword(const std::string& password);
bool verifyAdminPassword(const std::string& password, const std::string& stored);

std::string randomHex(size_t bytes);
// Lättläst slumpsträng (inga 0/O/1/l/I) – för setup-nätets lösenord.
std::string randomReadable(size_t length);
