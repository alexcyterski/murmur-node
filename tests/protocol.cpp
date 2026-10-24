// Host-side contract checks; no ESP32 or Arduino runtime required.
#include "../src/protocol.h"

#include <cassert>
#include <string>

int main() {
  assert(murmur::namePrefixBytes("") == 0);
  assert(murmur::namePrefixBytes("Bedroom") == 7);
  for (const std::string &character : {std::string("A"), std::string(u8"é"), std::string(u8"🐦")}) {
    std::string name;
    for (size_t i = 0; i < 48; ++i) name += character;
    assert(murmur::namePrefixBytes(name.c_str()) == name.size());
    const auto expected = name;
    name += character;
    assert(name.substr(0, murmur::namePrefixBytes(name.c_str())) == expected);
  }
  const std::string mixed = std::string(47, 'a') + u8"🐦" + "z";
  assert(murmur::namePrefixBytes(mixed.c_str()) == 51);
}
