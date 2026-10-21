// forza - the CPU the game's code was compiled for (FORZA_CPU_TARGET, cmake/CpuTarget.cmake, #220).
#pragma once

#include <cstdint>
#include <string>

namespace forza::cpu_target {

// The game exits with this code when the CPU lacks what the build needs; the launcher
// explains it (launcher/forza_launcher/game.py, CPU_EXIT_CODE).
constexpr int kExitCode = 76;

// The CPUID and XGETBV words that say whether code compiled for x86-64-v3 can run here.
struct CpuidWords {
  uint32_t max_leaf = 0;      // leaf 0, EAX
  uint32_t leaf1_ecx = 0;     // leaf 1, ECX
  uint32_t leaf7_ebx = 0;     // leaf 7 subleaf 0, EBX
  uint32_t max_ext_leaf = 0;  // leaf 0x80000000, EAX
  uint32_t ext1_ecx = 0;      // leaf 0x80000001, ECX
  uint64_t xcr0 = 0;          // XGETBV(0); read only when the OS enabled XSAVE (OSXSAVE)
};

// The x86-64-v3 features (and the x86-64-v2 ones below them) that are missing, as
// "AVX2, MOVBE"; empty when the CPU and OS have all of them. AVX counts only when the OS
// saves the AVX registers (XCR0), as a CPU can have it and the OS leave it off.
std::string MissingV3Features(const CpuidWords& words);

// This CPU's words.
CpuidWords ReadCpuid();

}  // namespace forza::cpu_target
