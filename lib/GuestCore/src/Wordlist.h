#pragma once
#include <cstddef>

namespace guest {

// Enkla, vanliga engelska ord: 3–7 bokstäver, gemener a–z, inga homofoner
// (red/read, sun/son …) och inget som kan läsas som stötande.
extern const char* const kWords[];
extern const size_t kWordCount;

}  // namespace guest
