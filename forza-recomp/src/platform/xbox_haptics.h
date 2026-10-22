#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace forza::telemetry {
// FH1X commands are acknowledged with FH1A, separately from FH1T telemetry.
struct XboxCommand { uint32_t sequence, sample; uint16_t left, right; };
inline bool DecodeXbox(const uint8_t* data, size_t size, uint64_t token, XboxCommand& out) {
  if (size != 32 || std::memcmp(data, "FH1X\1\0\40\0", 8)) return false;
  uint64_t nonce;
  uint32_t reserved;
  std::memcpy(&nonce, data+8, 8);
  std::memcpy(&reserved, data+28, 4);
  if (nonce != token || reserved) return false;
  std::memcpy(&out.sequence, data+16, 4);
  std::memcpy(&out.sample, data+20, 4);
  std::memcpy(&out.left, data+24, 2);
  std::memcpy(&out.right, data+26, 2);
  return true;
}
inline bool After(uint32_t next, uint32_t previous) {
  const uint32_t delta = next - previous;
  return delta && delta < 0x80000000u;
}
}
