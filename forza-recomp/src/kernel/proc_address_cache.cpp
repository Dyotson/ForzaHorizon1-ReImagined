// forza - XexGetProcedureAddress hands out one thunk per export and caller module.
//
// For a recompiled module the SDK answers each XexGetProcedureAddress(ordinal) with a new
// thunk address in the caller's 64 KB thunk reserve (UserModule::GetProcAddressByOrdinal ->
// FunctionDispatcher::AllocateThunk), and never reuses one. Title Update 4's executable
// looks the facade DLLs' exports up again and again (XMediaFacade_default), so within a
// second of starting it ran out ("Thunk address space exhausted for module at 82410000"),
// the next lookup failed ("XexGetProcedureAddress ordinal 6 ... not found") and the game
// aborted. Here the first answer for (module handle, ordinal, calling module) is kept and
// given again.

#include "../platform/guest_memory.h"
#include "../platform/sdk_import.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>

namespace {

std::mutex g_mutex;
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> g_cache;

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

}  // namespace

// NTSTATUS XexGetProcedureAddress(HANDLE module, DWORD ordinal, void** function)
extern "C" REX_FUNC(__imp__XexGetProcedureAddress) {
  static forza::GuestFunc sdk = forza::SdkImport("__imp__XexGetProcedureAddress");
  const uint32_t module = ctx.r3.u32;
  const uint32_t ordinal = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  // Thunks belong to the calling module: the executable (below 0x88000000) or a DLL
  // (one per 16 MB, 0x88000000 and up).
  const uint32_t caller = lr < 0x88000000 ? 0 : lr >> 24;
  // Names (pointers) are left to the SDK: it allocates nothing for them.
  const bool by_ordinal = (ordinal & 0xFFFF0000) == 0;
  const auto key = std::make_tuple(module, ordinal, caller);
  if (by_ordinal && out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
      Store32(forza::GuestPtr(base, out), it->second);
      ctx.r3.u64 = 0;
      return;
    }
  }
  sdk(ctx, base);
  if (by_ordinal && out && ctx.r3.u32 == 0) {
    const uint32_t ptr = Load32(forza::GuestPtr(base, out));
    if (ptr) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_cache.emplace(key, ptr);
    }
  }
}
