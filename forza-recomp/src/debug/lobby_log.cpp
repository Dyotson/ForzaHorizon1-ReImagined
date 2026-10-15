// forza - FORZA_LOBBY_LOG=1 logs a Find Game lobby's launch check and what feeds it (#14:
// a Playground Games lobby that sat at "WAITING FOR PLAYERS TO JOIN..." after a King round).
//
// The lobby update (sub_8287F670, every frame, world_events.cpp calls LobbyLogTick from its
// hook) runs the host's team/bucket check (sub_82875FB8) and, in lobby state 4, the launch
// check sub_82876D90. That one says ready when
//   humans present (sub_828754A0 counting sub_82865560: player state p[7] == 0, p[0] != 0)
//     >= the playlist's minPlayersToLaunch (+2016),
//   nobody still in the last round (counting sub_82865500: status p[6] 3, 4 or 5) == 0,
//   the bucket check's result (+2148) != 0 (or the mode is 10, Find Custom Game),
//   +2164 clear and *(+2156)+4 != 3
// (base offsets; Title Update 4 has the fields from the game options on 24 bytes later). The
// bucket check runs when +2152 is set (a player's status event, sub_8287E098 category 3) and
// only once the CPU driver count (p[7] == 1, sub_82865528) matches the session's
// (*(*(*0x832E4A84 + 24) + 40)); its result: 0 a bucket outside its MinPlayers/MaxPlayers
// (then the players are put in buckets again and checked once more), 1 teams 2 apart, 2 fine.
//
// Logged ("LOBBY:" lines): each peer's lobby state when it changes; for the host, the check's
// inputs sampled each second, logged when they change and every 10 s, with each player's
// state bytes; the lobby's events (sub_8287E098, category and code; frequent ones thinned);
// every player status change ("EVENT: status record", world_events.cpp; 1 is car select,
// 3 loading, 4 in game, 6 removed).

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

extern "C" REX_FUNC(__imp__sub_82876D90);
extern "C" REX_FUNC(__imp__sub_828754A0);
extern "C" REX_FUNC(__imp__sub_82865560);
extern "C" REX_FUNC(__imp__sub_8287E098);

namespace {

constexpr uint32_t kIsHumanPresent = FORZA_ADDR(0x82865560);
constexpr uint32_t kIsInRound = FORZA_ADDR(0x82865500);
constexpr uint32_t kIsAi = FORZA_ADDR(0x82865528);
constexpr uint32_t kSession = FORZA_ADDR(0x832E4A84);  // pointer; +24 -> +40 the CPU driver count

// Online game fields (world_events.cpp has the same).
constexpr uint32_t kMode = 0x28, kState = 0x2C, kInGame = 0x30, kLaunchTime = 0x4C, kLaunchTime2 = 0x50;
constexpr uint32_t kPlayers = 0x10C, kHost = 0x158;
constexpr uint32_t kPreset = FORZA_TU4 ? 0x1B8 : 0x1B4;
constexpr uint32_t kNextReady = FORZA_TU4 ? 0x490 : 0x484;
constexpr uint32_t kNextPreset = FORZA_TU4 ? 0x498 : 0x488;
constexpr uint32_t kRound = FORZA_TU4 ? 0x7F4 : 0x7DC;
constexpr uint32_t kMinPlayers = FORZA_TU4 ? 2040 : 2016;
constexpr uint32_t kBucketResult = FORZA_TU4 ? 2172 : 2148, kBucketDue = kBucketResult + 4;
constexpr uint32_t kStatePtr = kBucketResult + 8, kBlock = kBucketResult + 16;
constexpr uint32_t kRebalance = FORZA_TU4 ? 2156 : 2132;  // two counters the lobby compares

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_LOBBY_LOG");
    return v && *v && *v != '0';
  }();
  return on;
}

uint32_t Load32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = forza::GuestPtr(base, addr);
  return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

uint32_t CallGuest(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3, uint32_t r4) {
  PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

double Now() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration<double>(steady_clock::now() - start).count();
}

// Each player's state bytes, collected while LogCheck counts them.
thread_local std::string* collect = nullptr;

}  // namespace

namespace forza {
bool LobbyLogEnabled() { return Enabled(); }

// Called by world_events.cpp's lobby update hook on every peer: the lobby state when it changes.
void LobbyLogTick(uint8_t* base, uint32_t game) {
  if (!Enabled() || !game) return;
  uint8_t* g = GuestPtr(base, game);
  const uint32_t mode = Load32(base, game + kMode), state = Load32(base, game + kState);
  const uint32_t key = mode << 24 | state << 8 | (g[kHost] ? 2 : 0) | (Load32(base, game + kInGame) == 1);
  static uint32_t last = ~0u;
  if (key == last) return;
  last = key;
  REXLOG_INFO("LOBBY: t {:.1f} mode {} state {} host {} in game {} players {} preset {} next {}{} round {}",
              Now(), mode, state, g[kHost], Load32(base, game + kInGame), Load32(base, game + kPlayers),
              g[kPreset], g[kNextPreset], g[kNextReady] ? "" : " (not ready)",
              Load32(base, game + kRound) >> 16);
}
}  // namespace forza

// The host's launch check (r3 the online game); true when the lobby may start its intermission.
extern "C" REX_FUNC(sub_82876D90) {
  const uint32_t game = ctx.r3.u32;
  if (!Enabled()) return __imp__sub_82876D90(ctx, base);
  PPCContext before = ctx;
  __imp__sub_82876D90(ctx, base);
  const bool ready = ctx.r3.u8 != 0;
  static double last_sample = -1, last_log = -1;
  static std::string last_line;
  const double now = Now();
  if (now - last_sample < 1.0) return;
  last_sample = now;
  std::string players;
  collect = &players;
  const uint32_t humans = CallGuest(before, base, __imp__sub_828754A0, game, kIsHumanPresent);
  collect = nullptr;
  const uint32_t in_round = CallGuest(before, base, __imp__sub_828754A0, game, kIsInRound);
  const uint32_t ai = CallGuest(before, base, __imp__sub_828754A0, game, kIsAi);
  uint32_t session_ai = ~0u;
  if (const uint32_t s = Load32(base, kSession)) {
    if (const uint32_t s2 = Load32(base, s + 24)) session_ai = Load32(base, s2 + 40);
  }
  uint8_t* g = forza::GuestPtr(base, game);
  const uint32_t state_ptr = Load32(base, game + kStatePtr);
  const std::string line = fmt::format(
      "ready {} | humans {} >= min {} | in round {} | bucket result {} due {} (CPU {} session {}) | "
      "block {} | state {} | launch {:X}/{:X} | rebalance {}/{} | preset {} next {} | players{}",
      ready, humans, Load32(base, game + kMinPlayers), in_round, Load32(base, game + kBucketResult),
      g[kBucketDue], ai, int32_t(session_ai), g[kBlock], state_ptr ? int32_t(Load32(base, state_ptr + 4)) : -1,
      Load32(base, game + kLaunchTime), Load32(base, game + kLaunchTime2), Load32(base, game + kRebalance),
      Load32(base, game + kRebalance + 4), g[kPreset], g[kNextPreset], players);
  if (line != last_line || now - last_log >= 10.0) {
    REXLOG_INFO("LOBBY: t {:.1f} check: {}", now, line);
    last_line = line;
    last_log = now;
  }
}

// A player's state (r3): p[0] present, p[6] status (3-5: still in the last round), p[7] 1 for a CPU driver.
extern "C" REX_FUNC(sub_82865560) {
  if (collect) {
    const uint8_t* p = forza::GuestPtr(base, ctx.r3.u32);
    *collect += fmt::format(" [{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}]", p[0], p[1], p[2],
                            p[3], p[4], p[5], p[6], p[7]);
  }
  __imp__sub_82865560(ctx, base);
}

// The online game's event handler (r3 the game + 4, r4 category, r5 code, r6 data).
extern "C" REX_FUNC(sub_8287E098) {
  if (Enabled()) {
    static std::mutex mutex;
    static std::map<uint64_t, uint32_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t n = ++seen[uint64_t(ctx.r4.u32) << 32 | ctx.r5.u32];
    if (n <= 20 || (n & (n - 1)) == 0) {
      const uint32_t game = ctx.r3.u32 - 4;
      REXLOG_INFO("LOBBY: t {:.1f} event {} code {} (#{}) state {} due {} result {}", Now(), ctx.r4.u32,
                  ctx.r5.u32, n, Load32(base, game + kState), forza::GuestPtr(base, game)[kBucketDue],
                  Load32(base, game + kBucketResult));
    }
  }
  __imp__sub_8287E098(ctx, base);
}
