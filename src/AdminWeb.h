#pragma once
#include <GuestCore.h>
#include <WebServer.h>

#include <functional>
#include <string>

#include "Settings.h"

// Kopplingen mellan adminsidan och resten av firmwaren.
struct AdminContext {
  Settings* settings = nullptr;
  std::function<void(guest::AdminView&)> fillStatus;
  std::function<bool(std::string& message)> testUnifi;
  std::function<bool(std::string& fingerprint, std::string& error)> fetchFingerprint;
  std::function<void()> requestRotate;
  std::function<void(uint32_t delayMs)> scheduleRestart;
};

// Registrerar /admin och dess formulär på webbservern.
void adminBegin(WebServer& server, AdminContext ctx);

// Har någon varit inloggad på adminsidan de senaste `withinMs`?
bool adminRecentlyActive(uint32_t withinMs);
