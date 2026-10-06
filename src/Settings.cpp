#include "Settings.h"

#include <GuestCore.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include <cstdlib>
#include <vector>

static constexpr const char* kNamespace = "secrets";
static constexpr unsigned kIterations = 10000;

void settingsLoad(Settings& s) {
  Preferences p;
  p.begin(kNamespace, true);
  s.wifiPassword = p.getString("wifi", "").c_str();
  s.udmUser = p.getString("udmu", "").c_str();
  s.udmPassword = p.getString("udmp", "").c_str();
  s.fingerprint = p.getString("fp", "").c_str();
  s.adminHash = p.getString("admin", "").c_str();
  p.end();
}

void settingsSave(const Settings& s) {
  Preferences p;
  p.begin(kNamespace, false);
  p.putString("wifi", s.wifiPassword.c_str());
  p.putString("udmu", s.udmUser.c_str());
  p.putString("udmp", s.udmPassword.c_str());
  p.putString("fp", s.fingerprint.c_str());
  p.putString("admin", s.adminHash.c_str());
  p.end();
}

void settingsErase() {
  Preferences p;
  p.begin(kNamespace, false);
  p.clear();
  p.end();
}

std::string randomHex(size_t bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t b = static_cast<uint8_t>(esp_random());
    out += digits[b >> 4];
    out += digits[b & 0xF];
  }
  return out;
}

std::string randomReadable(size_t length) {
  static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
  guest::PasswordGenerator rng([] { return esp_random(); });
  std::string out;
  for (size_t i = 0; i < length; ++i) out += alphabet[rng.uniform(sizeof(alphabet) - 1)];
  return out;
}

static std::vector<uint8_t> fromHex(const std::string& hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
  }
  return out;
}

static std::string toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0xF];
  }
  return out;
}

static bool pbkdf2(const std::string& password, const std::vector<uint8_t>& salt, unsigned iterations,
                   uint8_t out[32]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                       reinterpret_cast<const unsigned char*>(password.data()),
                                       password.size(), salt.data(), salt.size(), iterations, 32,
                                       out) == 0;
}

std::string hashAdminPassword(const std::string& password) {
  const std::string saltHex = randomHex(16);
  uint8_t hash[32];
  if (!pbkdf2(password, fromHex(saltHex), kIterations, hash)) return "";
  return "pbkdf2-sha256$" + std::to_string(kIterations) + "$" + saltHex + "$" + toHex(hash, 32);
}

bool verifyAdminPassword(const std::string& password, const std::string& stored) {
  // pbkdf2-sha256$<iter>$<salt>$<hash>
  const size_t a = stored.find('$');
  const size_t b = stored.find('$', a + 1);
  const size_t c = stored.find('$', b + 1);
  if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return false;
  if (stored.substr(0, a) != "pbkdf2-sha256") return false;
  const unsigned iterations = static_cast<unsigned>(std::strtoul(stored.substr(a + 1, b - a - 1).c_str(), nullptr, 10));
  if (iterations < 1000) return false;
  uint8_t hash[32];
  if (!pbkdf2(password, fromHex(stored.substr(b + 1, c - b - 1)), iterations, hash)) return false;
  return guest::constantTimeEquals(toHex(hash, 32), stored.substr(c + 1));
}
