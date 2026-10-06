#include "Rotator.h"

#include <utility>

namespace guest {

Rotator::Rotator(WlanApi& api, PasswordGenerator& gen, std::string ssid, SleepFn sleep,
                 int verifyAttempts, uint32_t verifyDelayMs)
    : api_(api),
      gen_(gen),
      ssid_(std::move(ssid)),
      sleep_(std::move(sleep)),
      verifyAttempts_(verifyAttempts),
      verifyDelayMs_(verifyDelayMs) {}

bool Rotator::sync(std::string& current, std::string& error) {
  if (api_.readPassphrase(ssid_, current)) return true;
  error = api_.lastError();
  return false;
}

RotateResult Rotator::rotate(const std::string& previous) {
  RotateResult result;
  const std::string candidate = gen_.generate(previous);

  const bool written = api_.writePassphrase(ssid_, candidate);
  std::string writeError = written ? "" : api_.lastError();

  // Har skrivningen misslyckats räcker en kontroll; annars ges routern tid att tillämpa.
  const int attempts = written ? verifyAttempts_ : 1;
  std::string seen;
  std::string readError;
  for (int i = 0; i < attempts; ++i) {
    sleep_(verifyDelayMs_);
    if (!api_.readPassphrase(ssid_, seen)) {
      readError = api_.lastError();
      continue;
    }
    if (seen == candidate) {
      result.ok = true;
      result.password = candidate;
      return result;
    }
  }

  if (!written) {
    result.error = "skrivning misslyckades: " + writeError;
  } else if (!readError.empty() && seen.empty()) {
    result.error = "verifiering misslyckades: " + readError;
  } else {
    result.error = "verifiering misslyckades: routern har inte det nya lösenordet";
  }
  return result;
}

}  // namespace guest
