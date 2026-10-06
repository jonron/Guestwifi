#pragma once
#include <string>

namespace guest {

// Det rotationen behöver av routern. UnifiClient implementerar detta.
class WlanApi {
 public:
  virtual ~WlanApi() = default;
  // Läser aktuellt lösenord för SSID:t.
  virtual bool readPassphrase(const std::string& ssid, std::string& passphrase) = 0;
  // Skriver nytt lösenord för SSID:t.
  virtual bool writePassphrase(const std::string& ssid, const std::string& passphrase) = 0;
  virtual const std::string& lastError() const = 0;
};

}  // namespace guest
