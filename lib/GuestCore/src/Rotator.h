#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "PasswordGenerator.h"
#include "WlanApi.h"

namespace guest {

struct RotateResult {
  bool ok = false;
  std::string password;  // det nya, verifierade lösenordet (om ok)
  std::string error;
};

// Byter lösenord på gäst-WLAN:et och godkänner bytet först när routern
// läses tillbaka med det nya värdet. Skrivningen kan lyckas trots att svaret
// tappas (APs omprovisioneras), därför verifieras alltid mot routern.
class Rotator {
 public:
  using SleepFn = std::function<void(uint32_t ms)>;

  Rotator(WlanApi& api, PasswordGenerator& gen, std::string ssid, SleepFn sleep,
          int verifyAttempts = 12, uint32_t verifyDelayMs = 5000);

  // Läser det lösenord som faktiskt gäller i routern just nu.
  bool sync(std::string& current, std::string& error);

  RotateResult rotate(const std::string& previous);

 private:
  WlanApi& api_;
  PasswordGenerator& gen_;
  std::string ssid_;
  SleepFn sleep_;
  int verifyAttempts_;
  uint32_t verifyDelayMs_;
};

}  // namespace guest
