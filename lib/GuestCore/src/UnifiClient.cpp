#include "UnifiClient.h"

#include <ArduinoJson.h>

#include <utility>

namespace guest {

UnifiClient::UnifiClient(HttpTransport& http, std::string username, std::string password,
                         std::string site)
    : http_(http),
      username_(std::move(username)),
      password_(std::move(password)),
      site_(std::move(site)) {}

std::string UnifiClient::wlanconfPath() const {
  return "/proxy/network/api/s/" + site_ + "/rest/wlanconf";
}

void UnifiClient::captureCsrf(const HttpResponse& resp) {
  // UniFi OS roterar CSRF-token och skickar då en ny i X-Updated-CSRF-Token.
  std::string updated = resp.header("x-updated-csrf-token");
  if (!updated.empty()) {
    csrf_ = updated;
    return;
  }
  std::string csrf = resp.header("x-csrf-token");
  if (!csrf.empty()) csrf_ = csrf;
}

bool UnifiClient::login() {
  token_.clear();
  csrf_.clear();
  if (username_.empty()) {
    error_ = "inga UniFi-inloggningsuppgifter konfigurerade";
    return false;
  }

  JsonDocument doc;
  doc["username"] = username_;
  doc["password"] = password_;
  doc["remember"] = false;
  std::string body;
  serializeJson(doc, body);

  HttpRequest req{"POST", "/api/auth/login", {}, body};
  HttpResponse resp;
  std::string err;
  if (!http_.send(req, resp, err)) {
    error_ = "login: " + err;
    return false;
  }
  if (resp.status != 200) {
    error_ = "login: HTTP " + std::to_string(resp.status);
    return false;
  }
  for (const auto& cookie : resp.setCookies) {
    if (cookie.compare(0, 6, "TOKEN=") == 0) {
      token_ = cookie.substr(6, cookie.find(';') - 6);
    }
  }
  if (token_.empty()) {
    error_ = "login: ingen TOKEN-cookie i svaret";
    return false;
  }
  captureCsrf(resp);
  return true;
}

bool UnifiClient::callOnce(const std::string& method, const std::string& path,
                           const std::string& body, HttpResponse& resp) {
  HttpRequest req{method, path, {}, body};
  req.headers.emplace_back("Cookie", "TOKEN=" + token_);
  if (!csrf_.empty()) req.headers.emplace_back("X-CSRF-Token", csrf_);
  std::string err;
  if (!http_.send(req, resp, err)) {
    error_ = method + " " + path + ": " + err;
    return false;
  }
  captureCsrf(resp);
  return true;
}

bool UnifiClient::call(const std::string& method, const std::string& path, const std::string& body,
                       HttpResponse& resp) {
  if (token_.empty() && !login()) return false;
  if (!callOnce(method, path, body, resp)) return false;
  if (resp.status == 401 || resp.status == 403) {
    // Sessionen har gått ut – logga in igen och försök en gång till.
    if (!login()) return false;
    if (!callOnce(method, path, body, resp)) return false;
  }
  if (resp.status != 200) {
    error_ = method + " " + path + ": HTTP " + std::to_string(resp.status);
    if (resp.status == 401 || resp.status == 403) token_.clear();
    return false;
  }
  return true;
}

bool UnifiClient::findWlan(const std::string& ssid, std::string& id, std::string& passphrase) {
  HttpResponse resp;
  if (!call("GET", wlanconfPath(), "", resp)) return false;

  JsonDocument filter;
  filter["data"][0]["_id"] = true;
  filter["data"][0]["name"] = true;
  filter["data"][0]["x_passphrase"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, resp.body, DeserializationOption::Filter(filter))) {
    error_ = "wlanconf: ogiltig JSON";
    return false;
  }
  for (JsonObject wlan : doc["data"].as<JsonArray>()) {
    if (ssid == (wlan["name"] | "")) {
      id = wlan["_id"] | "";
      passphrase = wlan["x_passphrase"] | "";
      cachedSsid_ = ssid;
      cachedId_ = id;
      return !id.empty();
    }
  }
  error_ = "hittar inget WLAN med namnet " + ssid;
  return false;
}

bool UnifiClient::readPassphrase(const std::string& ssid, std::string& passphrase) {
  std::string id;
  return findWlan(ssid, id, passphrase);
}

bool UnifiClient::writePassphrase(const std::string& ssid, const std::string& passphrase) {
  std::string id = cachedSsid_ == ssid ? cachedId_ : "";
  if (id.empty()) {
    std::string ignored;
    if (!findWlan(ssid, id, ignored)) return false;
  }

  JsonDocument doc;
  doc["x_passphrase"] = passphrase;
  std::string body;
  serializeJson(doc, body);

  HttpResponse resp;
  if (!call("PUT", wlanconfPath() + "/" + id, body, resp)) {
    cachedId_.clear();  // slå upp _id på nytt nästa gång
    return false;
  }

  JsonDocument filter;
  filter["meta"]["rc"] = true;
  filter["meta"]["msg"] = true;
  JsonDocument result;
  deserializeJson(result, resp.body, DeserializationOption::Filter(filter));
  std::string rc = result["meta"]["rc"] | "";
  if (rc != "ok") {
    error_ = "PUT wlanconf: rc=" + rc + " " + (result["meta"]["msg"] | "");
    return false;
  }
  return true;
}

}  // namespace guest
