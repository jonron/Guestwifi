#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace guest {

// Källa till 32-bitars slumptal. På ESP32: esp_random() (hårdvaru-RNG).
using RandomSource = std::function<uint32_t()>;

// Hur lösenordet skrivs. Standard: "stocking_banana_43".
struct PasswordStyle {
  char separator = '_';
  bool capitalize = false;  // true ger "Stocking_Banana_43"
};

// Skapar lösenord av två olika ord ur ordlistan och två siffror 2–9
// (0/1 utesluts eftersom de förväxlas med O/l/I).
class PasswordGenerator {
 public:
  explicit PasswordGenerator(RandomSource rng, PasswordStyle style = PasswordStyle());

  // Ger aldrig samma lösenord som `previous`.
  std::string generate(const std::string& previous = "");

  // Likformigt slumptal i [0, n) utan modulo-bias.
  uint32_t uniform(uint32_t n);

 private:
  RandomSource rng_;
  PasswordStyle style_;
};

}  // namespace guest
