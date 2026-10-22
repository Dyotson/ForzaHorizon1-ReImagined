// forza - player 1's controller capabilities: the first SDK slot that has a controller.
//
// XamInputGetCapabilities(user, flags, caps) takes the user index in r3 and the flags in r4,
// XamInputGetCapabilitiesEx(unk, user, flags, caps) in r4 and r5. PPCContext does not keep
// r0-r31 in order (r3 comes first, then r0, r1, r2, r4...), so the registers are passed by
// name, never found by indexing from r0: an index read the flags as the user and the caps
// pointer as the flags, and the slot loop then wrote slot numbers over that pointer, so the
// game never got a wheel's capabilities (#219). Used by kernel/input_autopress.cpp and
// tested by tests/input_caps_test.cpp with a fake SDK.

#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace forza::input_caps {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kErrorSuccess = 0;
constexpr uint64_t kFlagAnyUser = 1ull << 30;

// The registers holding the user index and the flags.
struct Args {
  PPCRegister& user;
  PPCRegister& flags;
};
inline Args CapabilitiesArgs(PPCContext& ctx) { return {ctx.r3, ctx.r4}; }
inline Args CapabilitiesExArgs(PPCContext& ctx) { return {ctx.r4, ctx.r5}; }

// Calls the SDK for slots 0, 1, ... until one answers: the slot number in the user register and
// the flags without XINPUT_FLAG_ANYUSER. The user and flags registers are put back afterwards
// and r3 keeps the SDK's answer (for XamInputGetCapabilities the user register is r3 itself).
// True when a slot answered.
inline bool FirstSlot(GuestFunc sdk, PPCContext& ctx, uint8_t* base, Args args, uint32_t slots) {
  const uint64_t user = args.user.u64, flags = args.flags.u64;
  bool found = false;
  for (uint32_t slot = 0; slot < slots && !found; ++slot) {
    args.user.u64 = slot;
    args.flags.u64 = flags & ~kFlagAnyUser;
    sdk(ctx, base);
    found = ctx.r3.u32 == kErrorSuccess;
  }
  const uint64_t result = ctx.r3.u64;
  args.user.u64 = user;
  args.flags.u64 = flags;
  ctx.r3.u64 = result;
  return found;
}

}  // namespace forza::input_caps
