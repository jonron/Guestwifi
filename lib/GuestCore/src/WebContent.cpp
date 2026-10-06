#include "WebContent.h"

#include <ArduinoJson.h>

namespace guest {

std::string htmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

static void replaceAll(std::string& s, const std::string& from, const std::string& to) {
  for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
    s.replace(pos, from.size(), to);
  }
}

std::string renderPage(const std::string& tmpl, const PublicStatus& st) {
  std::string page = tmpl;
  replaceAll(page, "{{SSID}}", htmlEscape(st.ssid));
  replaceAll(page, "{{PASSWORD}}", htmlEscape(st.password.empty() ? "…" : st.password));
  return page;
}

std::string statusJson(const PublicStatus& st) {
  JsonDocument doc;
  doc["ssid"] = st.ssid;
  doc["password"] = st.password;
  doc["verified"] = st.verified;
  doc["updated"] = st.updated;
  doc["version"] = st.version;
  std::string out;
  serializeJson(doc, out);
  return out;
}

}  // namespace guest
