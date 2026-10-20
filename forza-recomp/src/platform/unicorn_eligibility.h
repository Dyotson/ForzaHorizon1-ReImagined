#pragma once

#include <cstdint>

namespace forza::unicorn {

// Local development experiment only. This is not a server entitlement.
enum class TestMode { Off, Deny, Granted };

constexpr bool IsUnicorn(uint32_t model) {
  switch (model) {
    case 320: case 326: case 330: case 1082: case 1197: case 1233: case 1488:
      return true;
    default: return false;
  }
}

constexpr bool TestDenies(TestMode mode, bool network, uint32_t online_mode,
                          bool local_player, uint32_t model) {
  return mode == TestMode::Deny && network && online_mode != 0 && local_player &&
         IsUnicorn(model);
}

constexpr bool Eligible(bool native_result, TestMode mode, bool network,
                        uint32_t online_mode, bool local_player, uint32_t model) {
  return native_result && !TestDenies(mode, network, online_mode, local_player, model);
}

}  // namespace forza::unicorn
