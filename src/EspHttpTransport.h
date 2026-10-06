#pragma once
#include <GuestCore.h>

#include <cstdint>
#include <string>

// HTTPS mot UDM:en via WiFiClientSecure. UDM:en har ett självsignerat
// certifikat, så i stället för CA-kedja kontrolleras certifikatets
// SHA-256-fingeravtryck (pinning). Tomt fingeravtryck = ingen kontroll.
class EspHttpTransport : public guest::HttpTransport {
 public:
  EspHttpTransport(std::string host, uint16_t port, std::string sha256Fingerprint);
  bool send(const guest::HttpRequest& req, guest::HttpResponse& resp, std::string& error) override;

 private:
  std::string host_;
  uint16_t port_;
  std::string fingerprint_;
};
