// forza - FORZA_THREADTRACE=1: log guest thread creation, affinity and priority calls.
//
// On the 360, threads pinned to the same hardware thread do not run at the same time,
// and a game can lean on that instead of locking. The runtime ignores affinities and
// priorities (as Xenia does by default), so such threads run in parallel here. This logs
// what the game asks for: each new thread (start address, KTHREAD, handle, flags) and
// each KeSetAffinityThread / KeSetBasePriorityThread with the thread it applies to.
//
// FORZA_THREAD_PRIORITY (default "normal" on Windows, no effect elsewhere): a guest priority
// request never raises a host thread above normal (lowering stays allowed). The SDK gives a
// thread created with flag 0x20 (the game's I/O worker, 82B317A8) THREAD_PRIORITY_ABOVE_NORMAL
// right in XThread::Create, regardless of ignore_thread_priorities; Linux refuses that change
// (EPERM) and Wine ignores it, so only native Windows ran with it (#76). "game" keeps the
// request. Both paths that can set a host priority are covered: thread creation (flags 0x20 /
// 0x40) and KeSetBasePriorityThread (SDK: only with ignore_thread_priorities off);
// KeSetPriorityThread and KeSetPriorityClassThread are SDK stubs.

#include "../platform/guest_memory.h"
#include "../platform/sdk_import.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_THREADTRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

uint32_t CurrentHandle() {
  if (!rex::system::XThread::IsInThread()) return 0;
  auto* t = rex::system::XThread::GetCurrentThread();
  return t ? t->handle() : 0;
}

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Return addresses of the calling guest frames (stack back chain, saved LR at -8).
std::string Callers(PPCContext& ctx, uint8_t* base, int depth) {
  std::string out = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t frame = ctx.r1.u32;
  for (int i = 0; i < depth && frame; ++i) {
    const uint32_t parent = Load32(forza::GuestPtr(base, frame));
    if (parent <= frame || parent - frame > 0x100000 || (parent & 7)) break;
    const uint32_t lr = Load32(forza::GuestPtr(base, parent - 8));
    if (lr < 0x82000000 || lr >= 0x90000000) break;
    out += fmt::format(" {:08X}", lr);
    frame = parent;
  }
  return out;
}

// Lowers a thread's host priority to normal when the game's request raised it.
void ClampHostPriority(rex::system::XThread* t, const char* via) {
#ifdef _WIN32
  static const bool clamp = [] {
    const char* v = std::getenv("FORZA_THREAD_PRIORITY");
    return !(v && (std::strcmp(v, "game") == 0 || std::strcmp(v, "0") == 0));
  }();
  if (!clamp || !t || !t->thread()) return;
  const int32_t host = t->thread()->priority();
  if (host <= rex::thread::ThreadPriority::kNormal) return;
  t->thread()->set_priority(rex::thread::ThreadPriority::kNormal);
  static std::mutex mu;
  static std::unordered_set<uint32_t> logged;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!logged.insert(t->handle()).second) return;
  }
  REXLOG_INFO("Thread priority: handle {:08X} asked for host priority {} via {}, kept at normal "
              "(FORZA_THREAD_PRIORITY=game keeps the request)",
              t->handle(), host, via);
#else
  (void)t;
  (void)via;
#endif
}

}  // namespace

// NTSTATUS ExCreateThread(HANDLE* handle, DWORD stack_size, DWORD* thread_id,
//     void* xapi_startup, void* start_address, void* start_context, DWORD creation_flags)
extern "C" REX_FUNC(__imp__ExCreateThread) {
  static GuestFunc sdk = forza::SdkImport("__imp__ExCreateThread");
  const uint32_t handle_ptr = ctx.r3.u32, stack = ctx.r4.u32, startup = ctx.r6.u32,
                 start = ctx.r7.u32, param = ctx.r8.u32, flags = ctx.r9.u32;
  if (sdk) sdk(ctx, base);
  uint32_t handle = handle_ptr ? Load32(forza::GuestPtr(base, handle_ptr)) : 0;
  uint32_t kthread = 0;
  if (auto* kernel = rex::system::kernel_state()) {
    if (auto t = kernel->object_table()->LookupObject<rex::system::XThread>(handle)) {
      kthread = t->guest_object();
      if (flags & 0x60) ClampHostPriority(t.get(), "creation flags");
    }
  }
  if (!Enabled()) return;
  REXLOG_INFO("THREADTRACE: ExCreateThread by {:08X}: start {:08X} (xapi startup {:08X}) param "
              "{:08X} stack {:#x} flags {:08X} -> handle {:08X} kthread {:08X} status {:08X}",
              CurrentHandle(), start, startup, param, stack, flags, handle, kthread, ctx.r3.u32);
}

// KAFFINITY KeSetAffinityThread(KTHREAD* thread, KAFFINITY affinity, KAFFINITY* previous)
extern "C" REX_FUNC(__imp__KeSetAffinityThread) {
  static GuestFunc sdk = forza::SdkImport("__imp__KeSetAffinityThread");
  const uint32_t kthread = ctx.r3.u32, affinity = ctx.r4.u32;
  const std::string from = Enabled() ? Callers(ctx, base, 3) : std::string();
  if (sdk) sdk(ctx, base);
  if (Enabled()) {
    REXLOG_INFO("THREADTRACE: KeSetAffinityThread by {:08X}: kthread {:08X} affinity {:#x} from {}",
                CurrentHandle(), kthread, affinity, from);
  }
}

// LONG KeSetBasePriorityThread(KTHREAD* thread, LONG increment)
extern "C" REX_FUNC(__imp__KeSetBasePriorityThread) {
  static GuestFunc sdk = forza::SdkImport("__imp__KeSetBasePriorityThread");
  const uint32_t kthread = ctx.r3.u32;
  const int32_t priority = ctx.r4.s32;
  const std::string from = Enabled() ? Callers(ctx, base, 3) : std::string();
  if (sdk) sdk(ctx, base);
#ifdef _WIN32
  if (kthread) {
    if (auto* kernel = rex::system::kernel_state()) {
      auto t = rex::system::XObject::GetNativeObject<rex::system::XThread>(
          kernel, forza::GuestPtr(base, kthread));
      ClampHostPriority(t.get(), "KeSetBasePriorityThread");
    }
  }
#endif
  if (Enabled()) {
    REXLOG_INFO("THREADTRACE: KeSetBasePriorityThread by {:08X}: kthread {:08X} priority {} from {}",
                CurrentHandle(), kthread, priority, from);
  }
}
