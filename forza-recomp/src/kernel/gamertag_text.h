// forza - gamertags that come from the network are untrusted text. The game draws them in
// fixed 16-byte fields, and a name of combining marks or wide glyphs would cover other
// players' screens, so only ASCII letters, digits and single spaces, at most 15, are kept
// (an Xbox 360 gamertag). The server cleans names too; this is the second line of defence.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace forza {

inline constexpr size_t kGamertagMax = 15;

inline std::string SanitizeGamertag(std::string_view raw) {
  std::string out;
  bool space = false;
  for (unsigned char c : raw) {
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (alnum) {
      const bool sep = space && !out.empty();
      if (out.size() + (sep ? 1 : 0) + 1 > kGamertagMax) break;
      if (sep) out += ' ';
      space = false;
      out += char(c);
    } else if (c == ' ' || c < 0x20 || c == 0x7F) {
      space = true;  // controls act as separators; anything non-ASCII is dropped
    }
  }
  return out;
}

// Writes the cleaned name into a guest buffer of `capacity` bytes: at most 15 characters,
// the rest zero, always null-terminated.
inline void StoreGamertag(uint8_t* dst, size_t capacity, std::string_view raw) {
  if (!dst || !capacity) return;
  std::memset(dst, 0, capacity);
  const std::string name = SanitizeGamertag(raw);
  std::memcpy(dst, name.data(), name.size() < capacity - 1 ? name.size() : capacity - 1);
}

}  // namespace forza
