#pragma once
#include <cstdint>
namespace rex::system {
class XThread {
 public:
  static bool IsInThread() { return false; }
  static uint32_t GetCurrentThreadHandle() { return 0; }
};
}
