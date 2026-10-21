#pragma once
#include "xthread.h"
namespace rex::system {
class ObjectTable {
 public:
  template <typename T> T* LookupObject(uint32_t) {
    static T thread;
    return &thread;
  }
};
class KernelState {
 public:
  ObjectTable* object_table() { return &objects_; }
 private:
  ObjectTable objects_;
};
inline KernelState* kernel_state() {
  static KernelState kernel;
  return &kernel;
}
}
