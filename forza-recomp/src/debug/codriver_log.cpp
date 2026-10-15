// forza - FORZA_CODRIVER_LOG=1 traces the Horizon Rally co-driver's pace notes (Title Update 4
// only; docs/dlc.md, #41). The addresses are Title Update 4's (tu_address_map: TU4 addresses).
//
// The chain, TU4 addresses:
// - sub_825BD4B0, the co-driver's update, walks the stage's phrases; when the car reaches one
//   (sub_825984C0) sub_825B8A28 shows its icon and, for each call of the phrase, asks the game
//   audio (*0x833CFF68, vtable +0x80) to play "CoDriver_<lang>/<group>" / <cue> from
//   audio\VO\CoDriverAudio.xml with VO parameters of type 2 (co-driver) and volume 1.
// - That request goes to the audio thread and into the VO queue (sub_82623C10), which looks
//   the FMOD event up (sub_82C63CC8: EventSystem::getGroup, EventGroup::getEvent) and queues
//   it. The queue's update (sub_82623B00) starts one item at a time (sub_82623920): the event's
//   volume is the item's volume times the audio manager's co-driver volume (+0x5DD4, set from
//   Options > Audio: the SFX volume when Rally Co-Driver is ON, 0 when OFF) and the
//   RallyCoDriver bus (+0x5E44); above 0 it applies the RallyCoDriverPlaying mixer snapshot.
//   The next item starts when the event stops playing.
// Logged: the triggers, the queued phrases, the event lookups' FMOD results, each start with
// the volumes it used, and the queue's state changes.

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#if FORZA_TU4

extern "C" REX_FUNC(__imp__sub_825B8A28);
extern "C" REX_FUNC(__imp__sub_82623C10);
extern "C" REX_FUNC(__imp__sub_82623920);
extern "C" REX_FUNC(__imp__sub_82623B00);
extern "C" REX_FUNC(__imp__sub_82C63CC8);

namespace {

constexpr uint32_t kAudioManager = 0x8352EA5C;  // the audio manager's pointer

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_CODRIVER_LOG");
    return v && *v && *v != '0';
  }();
  return on;
}

std::string Str(uint8_t* base, uint32_t addr) {
  if (addr < 0x10000) return "<null>";
  const char* p = reinterpret_cast<const char*>(forza::GuestPtr(base, addr));
  return std::string(p, strnlen(p, 200));
}

uint32_t Load32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = forza::GuestPtr(base, addr);
  return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

float LoadF(uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

}  // namespace

// r3 the co-driver, r4 the phrase.
extern "C" REX_FUNC(sub_825B8A28) {
  if (Enabled()) REXLOG_INFO("codriver: phrase {:08X} reached", ctx.r4.u32);
  __imp__sub_825B8A28(ctx, base);
}

// r3 the VO queue, r4 the event group's path, r5 the event, r6 the VO parameters.
extern "C" REX_FUNC(sub_82623C10) {
  if (Enabled())
    REXLOG_INFO("codriver: queue {:08X} '{}' '{}' (queued {})", ctx.r3.u32, Str(base, ctx.r4.u32),
                Str(base, ctx.r5.u32), Load32(base, ctx.r3.u32 + 0x24));
  __imp__sub_82623C10(ctx, base);
}

// Starts the queue's next item; true when it plays at a volume above 0.
extern "C" REX_FUNC(sub_82623920) {
  if (!Enabled()) return __imp__sub_82623920(ctx, base);
  const uint32_t queue = ctx.r3.u32;
  const uint32_t queued = Load32(base, queue + 0x24);
  __imp__sub_82623920(ctx, base);
  if (!queued) return;
  const uint32_t mgr = Load32(base, kAudioManager);
  REXLOG_INFO("codriver: start = {} (type {}, volume {:.2f}, co-driver {:.2f}, bus {:.2f})", ctx.r3.u32 & 0xFF,
              Load32(base, queue + 0x14), LoadF(base, queue + 0x18), LoadF(base, mgr + 0x5DD4),
              LoadF(base, mgr + 0x5E44));
}

// The queue's update: its state (0 idle, 1 playing) and length when they change.
extern "C" REX_FUNC(sub_82623B00) {
  if (!Enabled()) return __imp__sub_82623B00(ctx, base);
  const uint32_t queue = ctx.r3.u32;
  __imp__sub_82623B00(ctx, base);
  static std::mutex mutex;
  static std::map<uint32_t, uint64_t> last;
  const uint64_t now = uint64_t(Load32(base, queue + 8)) << 32 | Load32(base, queue + 0x24);
  std::lock_guard<std::mutex> lock(mutex);
  auto [it, added] = last.emplace(queue, now);
  if (!added && it->second == now) return;
  it->second = now;
  REXLOG_INFO("codriver: queue {:08X} state {} queued {}", queue, uint32_t(now >> 32), uint32_t(now));
}

// The event lookup: r4 the group's path, r5 the event; returns the FMOD result.
extern "C" REX_FUNC(sub_82C63CC8) {
  if (!Enabled()) return __imp__sub_82C63CC8(ctx, base);
  const std::string group = Str(base, ctx.r4.u32), event = Str(base, ctx.r5.u32);
  __imp__sub_82C63CC8(ctx, base);
  if (group.find("CoDriver") != std::string::npos)
    REXLOG_INFO("codriver: getEvent '{}' '{}' = {}", group, event, int32_t(ctx.r3.u32));
}

#endif  // FORZA_TU4
