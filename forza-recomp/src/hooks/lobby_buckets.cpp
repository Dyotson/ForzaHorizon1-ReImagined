// forza - a Find Game lobby whose team/role draw fails draws again (#14: a Playground Games
// lobby that sat at "WAITING FOR PLAYERS TO JOIN..." with both players in it).
//
// The host puts the lobby's players in teams and buckets (the roles: Mouse and Cat, King and
// Minion) with sub_8289B618 on the table at the online game + 2052 (TU4 + 2076; per team 5
// words: players, then each bucket's count). It clears the table, assigns every player
// (sub_8289B0D0: teams first, then each team's buckets in random order, sub_82888380 being
// std::random_shuffle over the CRT's rand()) and returns the bucket check (sub_82895018): 0
// when a team has a bucket outside its MinPlayers/MaxPlayers, 1 teams 2 apart, 2 fine. It
// runs as the lobby opens after a round (sub_82876130) and in the host's check
// (sub_82875FB8) after a player's status event. The launch check (sub_82876D90) needs a
// result other than 0, and nothing else runs the assignment again.
//
// Cat & Mouse with two players is two teams of one, each of which needs its Mouse
// (MinPlayers 1, MaxPlayers 1). Each player drew Mouse or Cat; the fix-up after a failed
// check (sub_8289B0D0's loop) moved one player only, so when the other drew Cat the table
// stayed "1 1 0 0 0 | 1 0 1 0 0", the result 0, and the lobby after a round waited for good
// (the first lobby of a session adds its players one at a time as they join, sub_8289B450).
// Running the host's check again in such a lobby drew again and started it. Here a 0 in a
// Find Game lobby (mode 7) is drawn again at once, up to kTries times, and logged once
// ("LOBBY: team/role draw ..."). Custom games (mode 10) launch without this result.

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <string>

extern "C" REX_FUNC(__imp__sub_8289B618);

namespace {

constexpr uint32_t kOnlineGame = FORZA_ADDR(0x83305CC4);  // pointer to the online game object
constexpr uint32_t kMode = 0x28, kHost = 0x158;
constexpr uint32_t kModeFindGame = 7;
constexpr uint32_t kTable = FORZA_TU4 ? 2076 : 2052;
constexpr int kTries = 32;

uint32_t Load32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = forza::GuestPtr(base, addr);
  return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

std::string Table(uint8_t* base, uint32_t table) {
  std::string out;
  for (uint32_t team = 0; team < 4; ++team) {
    if (team) out += " |";
    for (uint32_t i = 0; i < 5; ++i) out += fmt::format(" {}", int32_t(Load32(base, table + (team * 5 + i) * 4)));
  }
  return out;
}

}  // namespace

// sub_8289B618(table): assign the lobby's teams and buckets; returns the bucket check.
extern "C" REX_FUNC(sub_8289B618) {
  const PPCContext entry = ctx;
  const uint32_t table = ctx.r3.u32;
  __imp__sub_8289B618(ctx, base);
  if (ctx.r3.u32 != 0) return;
  const uint32_t game = Load32(base, kOnlineGame);
  if (!game || table != game + kTable || Load32(base, game + kMode) != kModeFindGame ||
      !forza::GuestPtr(base, game)[kHost]) {
    return;
  }
  const std::string first = Table(base, table);
  int tries = 1;
  while (ctx.r3.u32 == 0 && tries < kTries) {
    ctx = entry;
    __imp__sub_8289B618(ctx, base);
    ++tries;
  }
  REXLOG_INFO("LOBBY: team/role draw outside the buckets' limits (teams{}), drawn {} times: result {} (teams{})",
              first, tries, ctx.r3.u32, Table(base, table));
}
