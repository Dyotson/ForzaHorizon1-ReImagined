// forza - built-in CPU sampling profiler (perf/samply need perf_event_paranoid <= 1).
//
// "PROF:START" on the input FIFO (FORZA_AUTOPRESS_FIFO) starts sampling, "PROF:STOP"
// writes the samples to $FORZA_CPUPROF (default cpuprof.txt in the working directory).
// A process-wide ITIMER_PROF delivers SIGPROF to the thread that used the CPU every 1 ms
// of CPU time; the handler records that thread's id and program counter. The file has one
// line per sample site: "count tid thread-name module offset"; tools/cpuprof_report.py
// symbolizes and sums them per function and per thread (module offsets resolve with
// addr2line; the recompiled guest functions are the sub_XXXXXXXX symbols of forza).
// Only CPU time is sampled: threads that wait cost nothing here.

#include "cpu_profile.h"

#include <rex/logging.h>

#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace forza::cpuprof {

#if defined(__linux__) && defined(__x86_64__)
namespace {

struct Sample {
  uint64_t pc;
  uint32_t tid;
};

constexpr size_t kMaxSamples = 4u << 20;  // ~70 minutes of one busy thread at 1 kHz
Sample* samples = nullptr;
std::atomic<size_t> count{0};
std::atomic<bool> running{false};
std::mutex control_mutex;

void OnProf(int, siginfo_t*, void* uc) {
  if (!running.load(std::memory_order_relaxed)) return;
  const size_t i = count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxSamples) return;
  const auto* ctx = static_cast<const ucontext_t*>(uc);
  samples[i].pc = uint64_t(ctx->uc_mcontext.gregs[REG_RIP]);
  samples[i].tid = uint32_t(syscall(SYS_gettid));
}

std::string ThreadName(uint32_t tid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
  std::string name = "?";
  if (FILE* f = std::fopen(path, "r")) {
    char buf[64] = {};
    if (std::fgets(buf, sizeof(buf), f)) {
      name = buf;
      while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
      for (char& c : name) {
        if (c == ' ') c = '_';
      }
    }
    std::fclose(f);
  }
  return name;
}

}  // namespace

void Start() {
  std::lock_guard<std::mutex> lock(control_mutex);
  if (running) return;
  if (!samples) samples = new Sample[kMaxSamples];
  count = 0;
  struct sigaction sa = {};
  sa.sa_sigaction = OnProf;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);
  running = true;
  itimerval timer = {};
  timer.it_interval.tv_usec = 1000;
  timer.it_value.tv_usec = 1000;
  setitimer(ITIMER_PROF, &timer, nullptr);
  REXLOG_INFO("CPUPROF: sampling started");
}

void Stop() {
  std::lock_guard<std::mutex> lock(control_mutex);
  if (!running) return;
  itimerval timer = {};
  setitimer(ITIMER_PROF, &timer, nullptr);
  running = false;
  const size_t n = std::min(count.load(), kMaxSamples);
  // Aggregate by (tid, pc), then resolve each pc's module once.
  std::map<std::pair<uint32_t, uint64_t>, uint64_t> sites;
  for (size_t i = 0; i < n; ++i) ++sites[{samples[i].tid, samples[i].pc}];
  const char* path = std::getenv("FORZA_CPUPROF");
  FILE* f = std::fopen(path && *path ? path : "cpuprof.txt", "w");
  if (!f) return;
  std::map<uint32_t, std::string> names;
  for (const auto& [key, hits] : sites) {
    const auto [tid, pc] = key;
    auto it = names.find(tid);
    if (it == names.end()) it = names.emplace(tid, ThreadName(tid)).first;
    Dl_info info{};
    const char* module = "?";
    uint64_t offset = pc;
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
      module = info.dli_fname;
      offset = pc - reinterpret_cast<uint64_t>(info.dli_fbase);
    }
    std::fprintf(f, "%llu %u %s %s 0x%llx\n", (unsigned long long)hits, tid, it->second.c_str(),
                 module, (unsigned long long)offset);
  }
  std::fclose(f);
  REXLOG_INFO("CPUPROF: {} samples ({} sites) written to {}", n, sites.size(),
              path && *path ? path : "cpuprof.txt");
}
#else
void Start() { REXLOG_WARN("CPUPROF: only supported on Linux x86-64; use a host profiler"); }
void Stop() {}
#endif

}  // namespace forza::cpuprof
