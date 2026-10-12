#pragma once
#include <string>

#include "WlanApi.h"

namespace guest {

// Fristående testläge utan router: lösenordet "skrivs" bara i minnet.
// Används när inget UniFi-konto är inställt, så att schema, rotation och
// skyltsida kan provas innan UDM-kopplingen finns.
class LocalWlanApi : public WlanApi {
 public:
  explicit LocalWlanApi(std::string initial) : passphrase_(std::move(initial)) {}

  bool readPassphrase(const std::string&, std::string& passphrase) override {
    passphrase = passphrase_;
    return true;
  }
  bool writePassphrase(const std::string&, const std::string& passphrase) override {
    passphrase_ = passphrase;
    return true;
  }
  const std::string& lastError() const override { return error_; }

 private:
  std::string passphrase_;
  std::string error_;
};

}  // namespace guest
