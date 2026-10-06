#include "Http.h"

#include <cctype>
#include <cstdlib>

namespace guest {

static std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}

std::string serializeRequest(const HttpRequest& req, const std::string& host) {
  std::string out = req.method + " " + req.path + " HTTP/1.1\r\n";
  out += "Host: " + host + "\r\n";
  out += "Connection: close\r\n";
  out += "Accept: application/json\r\n";
  out += "User-Agent: guestwifi-esp32\r\n";
  for (const auto& h : req.headers) out += h.first + ": " + h.second + "\r\n";
  if (!req.body.empty() || req.method == "POST" || req.method == "PUT") {
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
  }
  out += "\r\n";
  out += req.body;
  return out;
}

static bool decodeChunked(const std::string& in, std::string& out, std::string& error) {
  size_t pos = 0;
  out.clear();
  for (;;) {
    size_t eol = in.find("\r\n", pos);
    if (eol == std::string::npos) {
      error = "chunked: saknar storleksrad";
      return false;
    }
    std::string sizeLine = in.substr(pos, eol - pos);
    size_t semi = sizeLine.find(';');
    if (semi != std::string::npos) sizeLine.resize(semi);
    char* end = nullptr;
    unsigned long size = std::strtoul(sizeLine.c_str(), &end, 16);
    if (end == sizeLine.c_str()) {
      error = "chunked: ogiltig storlek";
      return false;
    }
    pos = eol + 2;
    if (size == 0) return true;
    if (pos + size > in.size()) {
      error = "chunked: avklippt svar";
      return false;
    }
    out.append(in, pos, size);
    pos += size + 2;  // data + CRLF
  }
}

bool parseResponse(const std::string& raw, HttpResponse& resp, std::string& error) {
  resp = HttpResponse();
  size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    error = "ofullständiga headers";
    return false;
  }
  size_t lineEnd = raw.find("\r\n");
  std::string statusLine = raw.substr(0, lineEnd);
  if (statusLine.compare(0, 5, "HTTP/") != 0) {
    error = "ogiltig statusrad";
    return false;
  }
  size_t sp = statusLine.find(' ');
  resp.status = sp == std::string::npos ? 0 : std::atoi(statusLine.c_str() + sp + 1);
  if (resp.status < 100) {
    error = "ogiltig statuskod";
    return false;
  }

  size_t pos = lineEnd + 2;
  while (pos < headerEnd) {
    size_t e = raw.find("\r\n", pos);
    std::string line = raw.substr(pos, e - pos);
    pos = e + 2;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = lower(trim(line.substr(0, colon)));
    std::string value = trim(line.substr(colon + 1));
    if (name == "set-cookie") resp.setCookies.push_back(value);
    resp.headers[name] = value;
  }

  std::string body = raw.substr(headerEnd + 4);
  if (lower(resp.header("transfer-encoding")).find("chunked") != std::string::npos) {
    return decodeChunked(body, resp.body, error);
  }
  std::string cl = resp.header("content-length");
  if (!cl.empty()) {
    size_t n = static_cast<size_t>(std::strtoul(cl.c_str(), nullptr, 10));
    if (body.size() < n) {
      error = "avklippt svar";
      return false;
    }
    body.resize(n);
  }
  resp.body = std::move(body);
  return true;
}

}  // namespace guest
