#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

namespace forza::resource {

constexpr uint32_t GuestWord(uint32_t value) {
  if constexpr (std::endian::native == std::endian::little)
    return __builtin_bswap32(value);
  return value;
}

// The guest reader/writer lock counts readers in its low bits and marks a
// writer with bit 31. Its writer critical section must already be owned. A
// successful zero -> writer transition excludes readers that already loaded a
// cache pointer but haven't retained it yet. New readers wait in the guest's
// existing slow path until the original writer unlock wakes them.
inline bool TryClaimIdleCache(uint32_t& guest_counter) {
  static_assert(std::atomic_ref<uint32_t>::required_alignment <= alignof(uint32_t));
  uint32_t expected = 0;
  return std::atomic_ref<uint32_t>(guest_counter).compare_exchange_strong(
      expected, GuestWord(0x80000000u));
}

}  // namespace forza::resource
