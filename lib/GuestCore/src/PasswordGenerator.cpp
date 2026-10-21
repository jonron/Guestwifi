#include "PasswordGenerator.h"

#include <cctype>
#include <utility>

#include "Wordlist.h"

namespace guest {

PasswordGenerator::PasswordGenerator(RandomSource rng, PasswordStyle style)
    : rng_(std::move(rng)), style_(style) {}

uint32_t PasswordGenerator::uniform(uint32_t n) {
  if (n <= 1) return 0;
  // Förkasta värden i den ofullständiga sista "hinken".
  const uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
  uint32_t r;
  do {
    r = rng_();
  } while (r >= limit);
  return r % n;
}

static std::string styled(const char* word, bool capitalize) {
  std::string s(word);
  if (capitalize && !s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

std::string PasswordGenerator::generate(const std::string& previous) {
  for (;;) {
    const uint32_t a = uniform(kWordCount);
    uint32_t b;
    do {
      b = uniform(kWordCount);
    } while (b == a);

    const char sep = style_.separator;
    std::string pw = styled(kWords[a], style_.capitalize) + sep + styled(kWords[b], style_.capitalize) + sep;
    pw += static_cast<char>('2' + uniform(8));
    pw += static_cast<char>('2' + uniform(8));
    if (pw != previous) return pw;
  }
}

}  // namespace guest
