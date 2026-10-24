// forza - speed trap and speed zone leaderboards: a player's position past the rows the
// server sent (#288, src/platform/ambient_leaderboard.h).
//
// CAmbientChallengeLeaderboard (TU4 vtable 0x8208C27C) places a new score among the rows
// of its GetRows(view 5) answer with two methods: slot 9 (TU4 sub_82979E50, disc
// sub_828C8070) gives the place the player starts from, slot 10 (sub_82979E80 /
// sub_828C80A0) walks up from there while the score beats row[place - 2]. Both take the
// player's own row position as the start. sub_82990510 calls them when the answer arrives
// (the completion sub_82993130 on the main thread) and, when the score moved up, reads the
// overtaken player's xuid from row[start - 2] for NotifyRival. A position of 713 with 200
// rows read 136 KB past the object; the player crashed in free roam on unmapped memory.
//
// When the start would read past the rows held, these overrides start one place below the
// last row (where a player outside the rows is) and log it; otherwise the game's own code
// runs. The rows are never written.
// tu_address_map: TU4 addresses

#include "../platform/ambient_leaderboard.h"
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <bit>
#include <cstdint>
#include <mutex>

#if FORZA_TU4
#define AmbientStartPlace sub_82979E50
#define OriginalAmbientStartPlace __imp__sub_82979E50
#define AmbientNewPlace sub_82979E80
#define OriginalAmbientNewPlace __imp__sub_82979E80
#else
#define AmbientStartPlace sub_828C8070
#define OriginalAmbientStartPlace __imp__sub_828C8070
#define AmbientNewPlace sub_828C80A0
#define OriginalAmbientNewPlace __imp__sub_828C80A0
#endif
extern "C" REX_FUNC(OriginalAmbientStartPlace);
extern "C" REX_FUNC(OriginalAmbientNewPlace);

namespace {
namespace lb = forza::ambient_leaderboard;

constexpr uint32_t kRows = 0x58;         // 200 rows of kRowSize bytes
constexpr uint32_t kRowSize = 0xC0;
constexpr uint32_t kRowScore = 0x58;     // float
constexpr uint32_t kUserPosition = 0x9658;
constexpr uint32_t kRowCount = 0x9718;
constexpr uint32_t kBoard = 0x9724;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

lb::Start StartOf(const uint8_t* base, uint32_t object) {
  return lb::StartPlace(int32_t(Load32(base, object + kUserPosition)),
                        int32_t(Load32(base, object + kRowCount)));
}

// Once per answer (both methods see the same one), at most 20 times a session.
void Report(const uint8_t* base, uint32_t object, lb::Start start) {
  static std::mutex mutex;
  static uint32_t last_object, last_position, last_rows, reports;
  const uint32_t position = Load32(base, object + kUserPosition);
  const uint32_t rows = Load32(base, object + kRowCount);
  std::lock_guard lock(mutex);
  if (object == last_object && position == last_position && rows == last_rows) return;
  last_object = object, last_position = position, last_rows = rows;
  if (++reports > 20) return;
  REXLOG_WARN("SPEEDTRAP: board {} answer puts you at place {} with {} rows; placing new scores "
              "from place {} (object {:08X})",
              int32_t(Load32(base, object + kBoard)), int32_t(position), int32_t(rows),
              start.place, object);
}

}  // namespace

extern "C" REX_FUNC(AmbientStartPlace) {
  const lb::Start start = StartOf(base, ctx.r3.u32);
  if (!start.clamped) {
    OriginalAmbientStartPlace(ctx, base);
    return;
  }
  Report(base, ctx.r3.u32, start);
  ctx.r3.u64 = uint32_t(start.place);
}

extern "C" REX_FUNC(AmbientNewPlace) {
  const uint32_t object = ctx.r3.u32;
  const lb::Start start = StartOf(base, object);
  if (!start.clamped) {
    OriginalAmbientNewPlace(ctx, base);
    return;
  }
  Report(base, object, start);
  ctx.fpscr.disableFlushMode();  // as the game's loop: a denormal score is not read as 0
  const int32_t place = lb::NewPlace(start.place, ctx.f1.f64, [&](int32_t row) {
    return std::bit_cast<float>(Load32(base, object + kRows + uint32_t(row) * kRowSize + kRowScore));
  });
  ctx.r3.u64 = uint32_t(place);
}
