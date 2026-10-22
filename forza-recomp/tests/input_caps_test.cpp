// Player 1's controller capabilities (src/kernel/input_caps.h, #219): the slot loop must put the
// slot in the user register, leave the capabilities pointer alone and return the SDK's answer.
// Indexing the registers from r0 got all three wrong, because PPCContext keeps r3 first. Build
// against the SDK headers, from the repository root:
//   g++ -std=c++23 -msse4.1 -Wall -Wextra -Werror -I ~/rexglue-sdk-nightly/0.10.0.24/linux-amd64/include
//       -Iforza-recomp/src forza-recomp/tests/input_caps_test.cpp -o input-caps-test && ./input-caps-test
#include "kernel/input_caps.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace forza::input_caps;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// A fake SDK: records the arguments it gets and has a controller only in slot `connected`.
struct Call {
  uint64_t user, flags, caps;
};
std::vector<Call> calls;
uint32_t connected = 1;
bool ex = false;  // XamInputGetCapabilitiesEx: (unk, user, flags, caps) in r3-r6

void FakeSdk(PPCContext& ctx, uint8_t*) {
  const Call c = ex ? Call{ctx.r4.u64, ctx.r5.u64, ctx.r6.u64}
                    : Call{ctx.r3.u64, ctx.r4.u64, ctx.r5.u64};
  calls.push_back(c);
  ctx.r3.u64 = c.user == connected ? 0 : 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
}

constexpr uint64_t kCaps = 0x7000F120, kFlags = 1 | kFlagAnyUser;
}  // namespace

int main() {
  // XamInputGetCapabilities(XUSER_INDEX_ANY, XINPUT_FLAG_GAMEPAD | ANYUSER, caps).
  PPCContext ctx{};
  ctx.r3.u64 = 0xFF;
  ctx.r4.u64 = kFlags;
  ctx.r5.u64 = kCaps;
  Check(FirstSlot(FakeSdk, ctx, nullptr, CapabilitiesArgs(ctx), 4), "a slot answered");
  Check(calls.size() == 2, "slots tried until one answered");
  for (uint64_t slot = 0; slot < calls.size(); slot++) {
    Check(calls[slot].user == slot, "the slot is passed as the user index (r3)");
    Check(calls[slot].flags == 1, "the flags keep XINPUT_FLAG_GAMEPAD without ANYUSER (r4)");
    Check(calls[slot].caps == kCaps, "the capabilities pointer is left alone (r5)");
  }
  Check(ctx.r3.u64 == 0, "the SDK's success is returned, not the saved user index");
  Check(ctx.r4.u64 == kFlags && ctx.r5.u64 == kCaps, "flags and pointer are restored");

  // XamInputGetCapabilitiesEx(1, user 0, flags, caps): user and flags one register later.
  ex = true;
  calls.clear();
  connected = 2;
  ctx = PPCContext{};
  ctx.r3.u64 = 1;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = kFlags;
  ctx.r6.u64 = kCaps;
  Check(FirstSlot(FakeSdk, ctx, nullptr, CapabilitiesExArgs(ctx), 4), "Ex: a slot answered");
  Check(calls.size() == 3, "Ex: slots tried until one answered");
  for (uint64_t slot = 0; slot < calls.size(); slot++) {
    Check(calls[slot].user == slot && calls[slot].flags == 1 && calls[slot].caps == kCaps,
          "Ex: slot in r4, flags in r5, pointer in r6");
  }
  Check(ctx.r3.u64 == 0 && ctx.r4.u64 == 0 && ctx.r5.u64 == kFlags && ctx.r6.u64 == kCaps,
        "Ex: success returned, registers restored");

  // Nothing connected: every slot is tried and the last answer is returned.
  calls.clear();
  connected = 9;
  Check(!FirstSlot(FakeSdk, ctx, nullptr, CapabilitiesExArgs(ctx), 4), "no slot answered");
  Check(calls.size() == 4 && ctx.r3.u64 == 0x48F, "all slots tried, not connected returned");
  std::puts("input caps: all checks passed");
  return 0;
}
