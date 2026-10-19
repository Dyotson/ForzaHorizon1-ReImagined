#pragma once

#include <cstdint>
#include <mutex>

namespace forza::resource {

// Verified TU4 copies directly from cache slots. Other smart-pointer users
// retain their original implementation.
constexpr bool IsCacheCopyReturn(uint32_t address) {
  switch (address) {
    case 0x82D25CA4:
    case 0x82D2E898:
    case 0x82D2E924:
    case 0x82D2EA88:
    case 0x82D2EB08:
    case 0x82D2EC34:
    case 0x82D2EC70:
    case 0x82D33D78:
      return true;
    default:
      return false;
  }
}

class CachePinGate {
 public:
  template <typename Read, typename Retain, typename Publish, typename Release>
  void Copy(Read read, Retain retain, Publish publish, Release release) {
    const auto incoming = [&] {
      std::lock_guard lock(mutex_);
      auto value = read();
      if (value) retain(value);
      return value;
    }();
    // The new reference owns its lifetime now. Publication and the old
    // destination's Release may allocate/trim and must run outside this gate.
    const auto outgoing = publish(incoming);
    if (outgoing) release(outgoing);
  }

  std::unique_lock<std::mutex> TryEvict() {
    // The caller owns the global eviction-list lock. Never wait for a pin.
    return std::unique_lock(mutex_, std::try_to_lock);
  }

 private:
  std::mutex mutex_;
};

}  // namespace forza::resource
