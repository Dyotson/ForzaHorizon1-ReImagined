// forza - refuse to start on a CPU the build was not made for (#220).
//
// FORZA_CPU_TARGET=x86-64-v3 (cmake/CpuTarget.cmake) compiles the generated code for AVX2,
// FMA, BMI1/2, F16C, LZCNT and MOVBE. The launcher picks it when the build machine's CPU has
// all of them, but a built game can be copied to an older PC: there the first such
// instruction would end it with SIGILL, at some random point. This check runs before any of
// that code and exits with a message that says what to do instead.
//
// It runs before everything that could execute the generated code: on Linux from
// .preinit_array, which runs even before the shared libraries' initializers (the SDK runtime's
// static constructors can call inline functions this executable exports, and those may come
// from a generated-code object); on Windows a DLL never calls into the executable, so a
// constructor of priority 101 (.CRT$XCA00101, before every C++ initializer) is enough. The
// host code, this file included, keeps the SDK's baseline flags, so the check itself runs on
// any x86-64 CPU. Adapted from the early guard of #176 (guest FMA experiment, soconnor0919).

#include "cpu_target.h"

#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER) && !defined(__clang__)
#include <immintrin.h>
#include <intrin.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

namespace forza::cpu_target {

namespace {

#if defined(__x86_64__) || defined(_M_X64)
void Cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
#if defined(_MSC_VER) && !defined(__clang__)
  int r[4];
  __cpuidex(r, int(leaf), int(subleaf));
  for (int i = 0; i < 4; ++i) out[i] = uint32_t(r[i]);
#else
  __asm__ volatile("cpuid"
                   : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                   : "a"(leaf), "c"(subleaf));
#endif
}

uint64_t Xgetbv0() {
#if defined(_MSC_VER) && !defined(__clang__)
  return _xgetbv(0);
#else
  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
  return (uint64_t(hi) << 32) | lo;
#endif
}
#endif

struct Feature {
  const char* name;
  int word;  // 0 leaf1_ecx, 1 leaf7_ebx, 2 ext1_ecx
  int bit;
  bool avx_registers;  // uses the AVX registers: faults unless the OS saves them
};

// x86-64-v2 and v3 (the x86-64 psABI's levels) beyond the SSE2 every x86-64 CPU has.
constexpr Feature kFeatures[] = {
    {"SSE3", 0, 0, false},      {"SSSE3", 0, 9, false},  {"FMA", 0, 12, true},
    {"CMPXCHG16B", 0, 13, false}, {"SSE4.1", 0, 19, false}, {"SSE4.2", 0, 20, false},
    {"MOVBE", 0, 22, false},    {"POPCNT", 0, 23, false}, {"AVX", 0, 28, true},
    {"F16C", 0, 29, true},      {"BMI1", 1, 3, false},   {"AVX2", 1, 5, true},
    {"BMI2", 1, 8, false},      {"LAHF-SAHF", 2, 0, false}, {"LZCNT", 2, 5, false},
};

constexpr uint32_t kOsxsave = 1u << 27;
constexpr uint64_t kXmmYmmState = 0x6;  // XCR0 bits 1 (SSE) and 2 (AVX)

}  // namespace

std::string MissingV3Features(const CpuidWords& w) {
  const uint32_t words[3] = {w.leaf1_ecx, w.max_leaf >= 7 ? w.leaf7_ebx : 0u,
                             w.max_ext_leaf >= 0x80000001u ? w.ext1_ecx : 0u};
  const bool os_avx = (w.leaf1_ecx & kOsxsave) && (w.xcr0 & kXmmYmmState) == kXmmYmmState;
  std::string missing;
  for (const Feature& f : kFeatures) {
    const bool present = ((words[f.word] >> f.bit) & 1u) && (os_avx || !f.avx_registers);
    if (!present) {
      if (!missing.empty()) missing += ", ";
      missing += f.name;
    }
  }
  return missing;
}

CpuidWords ReadCpuid() {
  CpuidWords w;
#if defined(__x86_64__) || defined(_M_X64)
  uint32_t r[4];
  Cpuid(0, 0, r);
  w.max_leaf = r[0];
  if (w.max_leaf >= 1) {
    Cpuid(1, 0, r);
    w.leaf1_ecx = r[2];
  }
  if (w.max_leaf >= 7) {
    Cpuid(7, 0, r);
    w.leaf7_ebx = r[1];
  }
  Cpuid(0x80000000u, 0, r);
  w.max_ext_leaf = r[0];
  if (w.max_ext_leaf >= 0x80000001u) {
    Cpuid(0x80000001u, 0, r);
    w.ext1_ecx = r[2];
  }
  if (w.leaf1_ecx & kOsxsave) w.xcr0 = Xgetbv0();
#endif
  return w;  // not x86-64 (an ARM Mac): all zero
}

}  // namespace forza::cpu_target

#if defined(FORZA_CPU_TARGET_V3) && FORZA_CPU_TARGET_V3
namespace {

void CheckCpu() {
  const std::string missing =
      forza::cpu_target::MissingV3Features(forza::cpu_target::ReadCpuid());
  if (missing.empty()) return;
  char text[640];
  std::snprintf(text, sizeof(text),
                "This copy of Forza Horizon 1: ReImagined was built for a newer CPU (x86-64-v3) "
                "and cannot run on this one, which lacks: %s.\n"
                "Rebuild the game on this PC: in the launcher, Setup > Build.\n",
                missing.c_str());
  std::fputs(text, stderr);
  std::fflush(stderr);
#if defined(_WIN32)
  // A game started by hand has no console; the launcher explains the exit code itself.
  if (GetEnvironmentVariableA("FORZA_LAUNCHER_RELAUNCH", nullptr, 0) == 0) {
    MessageBoxA(nullptr, text, "Forza Horizon 1: ReImagined", MB_OK | MB_ICONERROR);
  }
#endif
  std::_Exit(forza::cpu_target::kExitCode);
}

#if defined(_WIN32)
__attribute__((constructor(101))) void CheckCpuAtStart() { CheckCpu(); }
#else
void CheckCpuAtStart(int, char**, char**) { CheckCpu(); }
__attribute__((section(".preinit_array"), used)) void (*const kCheckCpuAtStart)(int, char**,
                                                                                char**) =
    CheckCpuAtStart;
#endif

}  // namespace
#endif
