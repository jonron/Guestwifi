#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace guest {

struct HttpRequest {
  std::string method;
  std::string path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string> headers;  // nycklar i gemener; sista värdet vinner
  std::vector<std::string> setCookies;         // alla Set-Cookie-rader
  std::string body;

  std::string header(const std::string& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? std::string() : it->second;
  }
};

// Skickar en request och returnerar hela svaret. Implementeras med
// WiFiClientSecure på ESP32 och med POSIX-sockets i simulatorn.
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  // false = transportfel (anslutning, TLS, timeout); `error` beskriver felet.
  virtual bool send(const HttpRequest& req, HttpResponse& resp, std::string& error) = 0;
};

// Bygger en HTTP/1.1-request med "Connection: close".
std::string serializeRequest(const HttpRequest& req, const std::string& host);

// Tolkar ett komplett HTTP/1.1-svar (Content-Length, chunked eller läs-till-stängning).
bool parseResponse(const std::string& raw, HttpResponse& resp, std::string& error);

}  // namespace guest
