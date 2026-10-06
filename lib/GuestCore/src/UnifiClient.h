#pragma once
#include <string>

#include "Http.h"
#include "WlanApi.h"

namespace guest {

// Klient för UniFi Network på UniFi OS (UDM Pro Max), via det lokala API:t:
//   POST /api/auth/login                                    -> cookie TOKEN + X-CSRF-Token
//   GET  /proxy/network/api/s/<site>/rest/wlanconf          -> lista WLAN (inkl. x_passphrase)
//   PUT  /proxy/network/api/s/<site>/rest/wlanconf/<_id>    {"x_passphrase": "..."}
// Loggar in vid behov och gör om anropet en gång om sessionen gått ut (401).
class UnifiClient : public WlanApi {
 public:
  UnifiClient(HttpTransport& http, std::string username, std::string password,
              std::string site = "default");

  bool readPassphrase(const std::string& ssid, std::string& passphrase) override;
  bool writePassphrase(const std::string& ssid, const std::string& passphrase) override;
  const std::string& lastError() const override { return error_; }

 private:
  bool login();
  bool call(const std::string& method, const std::string& path, const std::string& body,
            HttpResponse& resp);
  bool callOnce(const std::string& method, const std::string& path, const std::string& body,
                HttpResponse& resp);
  bool findWlan(const std::string& ssid, std::string& id, std::string& passphrase);
  void captureCsrf(const HttpResponse& resp);
  std::string wlanconfPath() const;

  HttpTransport& http_;
  std::string username_, password_, site_;
  std::string token_, csrf_;
  std::string cachedSsid_, cachedId_;
  std::string error_;
};

}  // namespace guest
