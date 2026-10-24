#pragma once
#include <stddef.h>

namespace murmur {

constexpr size_t kNameMaxChars = 48;

// Arduino String::substring counts bytes. Keep complete UTF-8 characters so
// the limit matches core's Unicode string limit, including non-ASCII names.
inline size_t namePrefixBytes(const char *text) {
  size_t bytes = 0, chars = 0;
  while (text[bytes]) {
    if ((static_cast<unsigned char>(text[bytes]) & 0xC0) != 0x80) {
      if (chars == kNameMaxChars) break;
      ++chars;
    }
    ++bytes;
  }
  return bytes;
}

}  // namespace murmur
