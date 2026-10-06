#include "EspHttpTransport.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <utility>

static constexpr uint32_t kConnectTimeoutMs = 10000;
static constexpr uint32_t kResponseTimeoutMs = 15000;
static constexpr size_t kMaxResponseBytes = 96 * 1024;

EspHttpTransport::EspHttpTransport(std::string host, uint16_t port, std::string sha256Fingerprint)
    : host_(std::move(host)), port_(port), fingerprint_(std::move(sha256Fingerprint)) {}

bool EspHttpTransport::send(const guest::HttpRequest& req, guest::HttpResponse& resp,
                            std::string& error) {
  WiFiClientSecure client;
  client.setInsecure();  // kedjan valideras inte; fingeravtrycket kontrolleras nedan
  client.setHandshakeTimeout(kConnectTimeoutMs / 1000);
  if (!client.connect(host_.c_str(), port_, kConnectTimeoutMs)) {
    error = "kan inte ansluta till " + host_;
    return false;
  }
  if (!fingerprint_.empty() && !client.verify(fingerprint_.c_str(), nullptr)) {
    client.stop();
    error = "UDM-certifikatets fingeravtryck stämmer inte";
    return false;
  }

  const std::string raw = guest::serializeRequest(req, host_);
  if (client.write(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) != raw.size()) {
    client.stop();
    error = "skrivfel";
    return false;
  }

  std::string in;
  uint8_t buf[512];
  const uint32_t start = millis();
  while (millis() - start < kResponseTimeoutMs) {
    const int avail = client.available();
    if (avail > 0) {
      const int n = client.read(buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
      if (n > 0) in.append(reinterpret_cast<char*>(buf), n);
      if (in.size() > kMaxResponseBytes) {
        client.stop();
        error = "svaret är för stort";
        return false;
      }
    } else if (!client.connected()) {
      break;  // "Connection: close" – servern är klar
    } else {
      delay(5);
    }
  }
  client.stop();
  if (in.empty()) {
    error = "inget svar (timeout)";
    return false;
  }
  return guest::parseResponse(in, resp, error);
}
