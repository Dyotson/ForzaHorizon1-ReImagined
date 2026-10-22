// The x86-64-v3 check of src/platform/cpu_target.cpp (#220). Build and run (x86-64):
//   c++ -std=c++17 forza-recomp/tests/cpu_target_test.cpp forza-recomp/src/platform/cpu_target.cpp && ./a.out
// With -DFORZA_CPU_TARGET_V3=1 the game's guard runs too, before main (exit code 76 on a CPU without x86-64-v3).
#include "../src/platform/cpu_target.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

using forza::cpu_target::CpuidWords;
using forza::cpu_target::MissingV3Features;

static uint32_t Bits(std::initializer_list<int> bits) {
  uint32_t v = 0;
  for (int b : bits) v |= 1u << b;
  return v;
}

// A Haswell / Zen class CPU: every x86-64-v3 feature, the AVX state enabled by the OS.
static CpuidWords V3Cpu() {
  CpuidWords w;
  w.max_leaf = 0x10;
  // SSE3, SSSE3, FMA, CMPXCHG16B, SSE4.1, SSE4.2, MOVBE, POPCNT, XSAVE, OSXSAVE, AVX, F16C
  w.leaf1_ecx = Bits({0, 9, 12, 13, 19, 20, 22, 23, 26, 27, 28, 29});
  w.leaf7_ebx = Bits({3, 5, 8});  // BMI1, AVX2, BMI2
  w.max_ext_leaf = 0x80000020;
  w.ext1_ecx = Bits({0, 5});      // LAHF-SAHF, LZCNT
  w.xcr0 = 0x7;                   // x87, SSE, AVX state
  return w;
}

int main() {
  assert(MissingV3Features(V3Cpu()).empty());

  // Ivy Bridge: AVX and F16C but no AVX2, FMA, BMI, MOVBE or LZCNT.
  CpuidWords ivy = V3Cpu();
  ivy.leaf1_ecx &= ~((1u << 12) | (1u << 22));
  ivy.leaf7_ebx &= ~((1u << 3) | (1u << 5) | (1u << 8));
  ivy.ext1_ecx &= ~(1u << 5);
  assert(MissingV3Features(ivy) == "FMA, MOVBE, BMI1, AVX2, BMI2, LZCNT");

  // The OS does not save the AVX registers: everything that uses them is missing.
  CpuidWords no_os = V3Cpu();
  no_os.xcr0 = 0x3;
  assert(MissingV3Features(no_os) == "FMA, AVX, F16C, AVX2");
  CpuidWords no_osxsave = V3Cpu();
  no_osxsave.leaf1_ecx &= ~(1u << 27);
  assert(MissingV3Features(no_osxsave) == "FMA, AVX, F16C, AVX2");

  // A hypervisor that exposes AVX2 but hides MOVBE (some virtual CPU models).
  CpuidWords vm = V3Cpu();
  vm.leaf1_ecx &= ~(1u << 22);
  assert(MissingV3Features(vm) == "MOVBE");

  // Leaves the CPU does not have count as zero, whatever the words say.
  CpuidWords old = V3Cpu();
  old.max_leaf = 6;
  old.max_ext_leaf = 0x80000000;
  assert(MissingV3Features(old) == "BMI1, AVX2, BMI2, LAHF-SAHF, LZCNT");

  // This machine: just print what it lacks.
  const std::string here = MissingV3Features(forza::cpu_target::ReadCpuid());
  std::printf("this CPU: %s\n", here.empty() ? "x86-64-v3" : ("lacks " + here).c_str());
  std::puts("cpu_target_test: ok");
  return 0;
}
