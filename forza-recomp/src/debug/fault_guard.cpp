// forza - stop on a fault nothing handles instead of re-running it forever.
//
// The runtime's signal handler offers a fault to its handlers (MMIO ranges, GPU
// write watches); when none takes it, it returns and the faulting instruction runs
// again. A null read in recompiled code then faults forever, and the runtime logs
// "Unhandled guest access violation" each time: 4 MB of log a second (a 260 MB log in
// one minute when a copied save was loaded). A trap (__builtin_trap, e.g. a switch case
// out of range) loops the same way.
//
// This handler is installed after the runtime's, so it only sees faults nobody took.
// It logs the fault once with the host call stack as module+offset (resolve with
// `addr2line -f -e forza-recomp/out/build/linux/forza 0xOFFSET`; frames in sub_XXXXXXXX
// are the recompiled guest functions; on Windows the pc and the frames are forza.exe+offset,
// for the same lookup in the released executable) and exits with status 134. No core dump: one of a
// process mapping 5+ GB of guest memory takes minutes and gigabytes. FORZA_FAULT=wait
// parks the thread instead, so gdb can attach (gdb -p PID, then `thread apply all bt`).
//
// LogGuestThreads (called when the window closes) logs every guest thread's last call site
// and call chain, so the log of a game that hangs without a fault shows where it waits.

#include "../platform/guest_memory.h"

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#endif
#include <csignal>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <set>
#include <string>
#include <thread>

namespace forza {

namespace {

constexpr uint64_t kGuestVirtualBase = 0x100000000ull;  // Linux: guest X is host base + X

bool Wait() {
  const char* v = std::getenv("FORZA_FAULT");
  return v && std::strcmp(v, "wait") == 0;
}

// "module+0xOFFSET" for a host code address (Windows: the image the address is in, so a
// player's crash log maps onto the released forza.exe; elsewhere the plain address).
std::string HostAddr(uint64_t pc) {
#ifdef _WIN32
  HMODULE module = nullptr;
  wchar_t path[MAX_PATH];
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(pc), &module) &&
      GetModuleFileNameW(module, path, MAX_PATH)) {
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    std::string narrow;
    for (; *name; ++name) narrow += *name < 128 ? char(*name) : '?';
    return fmt::format("{}+0x{:X}", narrow, pc - reinterpret_cast<uint64_t>(module));
  }
#endif
  return fmt::format("0x{:X}", pc);
}

void LogStack() {
#ifdef _WIN32
  void* frames[48];
  const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    REXLOG_ERROR("  #{} {}", i, HostAddr(reinterpret_cast<uint64_t>(frames[i])));
  }
#else
  void* frames[48];
  const int n = backtrace(frames, 48);
  for (int i = 0; i < n; ++i) {
    Dl_info info{};
    if (dladdr(frames[i], &info) && info.dli_fname) {
      const char* name = std::strrchr(info.dli_fname, '/');
      REXLOG_ERROR("  #{} {}+0x{:X}{}{}", i, name ? name + 1 : info.dli_fname,
                   reinterpret_cast<uintptr_t>(frames[i]) -
                       reinterpret_cast<uintptr_t>(info.dli_fbase),
                   info.dli_sname ? " " : "", info.dli_sname ? info.dli_sname : "");
    } else {
      REXLOG_ERROR("  #{} {}", i, frames[i]);
    }
  }
#endif
}

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Whether the guest word at `address` can be read: its page is allocated and readable.
// A thread that has exited (or one whose context still holds an old r1) can leave a
// stack pointer into a released stack; reading it faulted (#79: closing the window
// logged "read of guest 0x708E0003" from LogGuestThreads and aborted).
bool GuestReadable(uint32_t address) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return false;
  auto* heap = kernel->memory()->LookupHeap(address);
  uint32_t protect = 0;
  return heap && heap->QueryProtect(address, &protect) &&
         (protect & rex::memory::kMemoryProtectRead) &&
         heap->QueryProtect(address + 3, &protect) &&
         (protect & rex::memory::kMemoryProtectRead);
}

// Return addresses along a guest stack's back chain (saved LR at back chain - 8), from
// the frame at r1 outwards. Stops at the first link that does not look like one, or
// that points at memory that is not readable.
std::string GuestChain(uint8_t* base, uint32_t frame, int max_frames) {
  std::string chain;
  for (int i = 0; i < max_frames && frame; ++i) {
    if (frame < 0x40000000 || frame >= 0x80000000) break;  // guest stacks (0x7xxxxxxx) only
    if ((frame & 3) || !GuestReadable(frame)) break;
    const uint32_t parent = Load32(forza::GuestPtr(base, frame));
    if (parent <= frame || parent - frame > 0x100000 || (parent & 7)) break;
    if (!GuestReadable(parent - 8)) break;
    const uint32_t lr = Load32(forza::GuestPtr(base, parent - 8));
    if (lr < 0x82000000 || lr >= 0x90000000) break;
    chain += fmt::format(" {:08X}", lr);
    frame = parent;
  }
  return chain;
}

// The guest registers of the faulting thread (as last stored in its PPCContext: values
// the recompiled code keeps in host registers may be newer) and its guest call chain
// from the stack back-chain (saved LR at back chain - 8).
void LogGuestState() {
  if (!rex::system::XThread::IsInThread()) return;
  auto* thread = rex::system::XThread::GetCurrentThread();
  auto* state = thread ? thread->thread_state() : nullptr;
  PPCContext* ctx = state ? state->context() : nullptr;
  auto* kernel = rex::system::kernel_state();
  if (!ctx || !kernel) return;
  uint8_t* base = kernel->memory()->virtual_membase();
  std::string regs;
  for (int i = 0; i < 32; ++i) {
    regs += fmt::format("{}r{}={:08X}", i % 8 ? " " : (i ? "\n    " : "    "), i,
                        (&ctx->r0)[i].u32);
  }
  REXLOG_ERROR("Guest thread {:08X} registers (lr={:08X} ctr={:08X}):\n{}",
               thread->handle(), uint32_t(ctx->lr), ctx->ctr.u32, regs);
  REXLOG_ERROR("Guest call chain (return addresses):{}", GuestChain(base, ctx->r1.u32, 24));
}

bool OnUnhandledFault(rex::arch::Exception* ex, void* /*data*/) {
  using Code = rex::arch::Exception::Code;
  if (ex->code() == Code::kAccessViolation) {
    const uint64_t addr = ex->fault_address();
    const bool guest = addr >= kGuestVirtualBase && addr < kGuestVirtualBase + 0x100000000ull;
    REXLOG_CRITICAL("Unhandled {} fault at host 0x{:X}{} (pc {})",
                    ex->access_violation_operation() ==
                            rex::arch::Exception::AccessViolationOperation::kWrite
                        ? "write"
                        : "read",
                    addr, guest ? fmt::format(" = guest 0x{:08X}", uint32_t(addr)) : "",
                    HostAddr(ex->pc()));
  } else {
    REXLOG_CRITICAL("Unhandled illegal instruction (trap) at pc {}", HostAddr(ex->pc()));
  }
  LogStack();
  LogGuestState();
  if (Wait()) {
    REXLOG_CRITICAL("FORZA_FAULT=wait: thread parked for a debugger");
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  }
  std::_Exit(134);
}

// A REX_FATAL (e.g. "Call to invalid or unregistered function at guest address 0", a
// call through a null or freed object's vtable) logs one line and aborts: log where the
// guest was, too (its LR is the indirect call's return address).
void OnAbort(int sig) {
  std::signal(sig, SIG_DFL);
  REXLOG_CRITICAL("Aborted (signal {})", sig);
  LogStack();
  LogGuestState();
  if (auto logger = rex::GetLogger()) logger->flush();
  std::_Exit(134);
}

}  // namespace

// Where every guest thread is when the player closes the window (#76): a game that loads
// forever logs nothing else, so this is what a hang report shows. One line per thread: its
// handle, start address, the guest return address of its last call (lr: the call site of a
// kernel wait it is blocked in) and its call chain, as last stored in its PPCContext (a
// thread running guest code without calls may be further on). Read without stopping the
// threads, so a running thread's chain can be cut short.
void LogGuestThreads(const char* why) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->object_table()) return;
  uint8_t* base = kernel->memory()->virtual_membase();
  auto threads = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
  REXLOG_INFO("FORZA: guest threads {}:", why);
  std::set<rex::system::XThread*> seen;  // a thread is listed once per handle
  for (auto& thread : threads) {
    if (!thread || !thread->is_guest_thread() || !seen.insert(thread.get()).second) continue;
    auto* state = thread->thread_state();
    PPCContext* ctx = state ? state->context() : nullptr;
    if (!ctx) continue;
    std::string host;  // the [tNNNN] of its log lines (Windows thread ids; not on Linux)
#ifdef _WIN32
    if (auto* t = thread->thread()) host = fmt::format(" t{}", t->system_id());
#endif
    REXLOG_INFO("  thread {:08X}{}{} start {:08X} lr {:08X} chain{}", thread->handle(), host,
                thread->main_thread() ? " (main)" : "", thread->creation_params()->start_address,
                uint32_t(ctx->lr), GuestChain(base, ctx->r1.u32, 16));
  }
}

void InstallFaultGuard() {
  rex::arch::ExceptionHandler::Install(OnUnhandledFault, nullptr);
  std::signal(SIGABRT, OnAbort);
}

}  // namespace forza
