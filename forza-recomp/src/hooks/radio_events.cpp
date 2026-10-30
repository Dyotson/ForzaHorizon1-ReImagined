// Opt-in radio preservation for career/Rally race-opening music cues.
//
// CPrepareCutscene's race-specific path resolves a fully qualified native key.
// Only its first successful lookup of an exact pre-race canonical name binds a
// record. Regional/shared fallbacks and unknown/story/post-race names confer no
// authority. An accepted current load invalidates every old binding before its
// native copy runs; publication after the copy makes reentrant observations fail
// closed. The request type must also agree with the native race settings.
//
// A bound record must be in the native running state (actual/desired phase 3),
// with its own live timeline. Only that record's timed music callback and exact
// helper enqueue/marker callsites may redirect. Nested ineligible calls mask TLS
// authority. No lock spans a native call; host binding state is synchronized.
//
// The original helper still owns dialogue, DJ/lock cleanup and its own string.
// Replace only its by-value deferred event-music command with the native Free
// Roam command, including its normal one-second fade and TU4 context flag. Once
// queued, the matching marker clears this request's forced-station attribution
// even if a new load starts; normal native queue teardown remains authoritative.
// Default off calls each original unchanged. CPU contract fixtures do not prove
// native output or Off behavior; those require bounded game captures.
#include "radio_event_bindings.h"
#include "../platform/guest_memory.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(radio_keep_in_events, false, "Audio", "Keep selected radio during career/Rally race-opening music cues, including Off");
extern "C" REX_FUNC(OriginalEventRequestCopy);
extern "C" REX_FUNC(OriginalRaceCutsceneLookup);
extern "C" REX_FUNC(OriginalCutsceneLookup);
extern "C" REX_FUNC(OriginalCutsceneMusicTrigger);
extern "C" REX_FUNC(OriginalStartEventMusic);
extern "C" REX_FUNC(OriginalQueueEventMusic);
extern "C" REX_FUNC(OriginalMarkScriptedStation);
extern "C" REX_FUNC(QueueFreeRoam);
#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4
extern "C" REX_FUNC(FreeRoamFlag);
#endif
extern "C" REX_FUNC(DestroyOwnedString);

namespace {
using namespace forza::radio_event_native;
bool Enabled() { static const bool enabled = REXCVAR_GET(radio_keep_in_events); return enabled; }
uint32_t Read(uint8_t* base, uint32_t a) {
  const auto* p = forza::GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
std::optional<std::string> String(uint8_t* base, uint32_t a) {
  if (!a) return {};
  const auto size = Read(base, a + 16), capacity = Read(base, a + 20);
  if (!size || size > 255 || size > capacity) return {};
  const auto data = capacity >= 16 ? Read(base, a) : a;
  if (!data) return {};
  const auto* p = forza::GuestPtr(base, data);
  std::string result(reinterpret_cast<const char*>(p), size);
  if (std::any_of(result.begin(), result.end(), [](unsigned char c) { return c < 32 || c > 126; })) return {};
  return result;
}
bool Canonical(const std::string& name) {
  return name == "prerace_intro" || name == "prerace_countdown" || name == "prerace_looping_01";
}
struct Token { uint8_t* base; uint32_t manager; uint64_t epoch; };
struct Binding { Token token; uint32_t record; std::string full_key; };
struct State {
  std::mutex mutex;
  uint8_t* base = nullptr;
  uint32_t manager = 0;
  uint64_t epoch = 0;
  unsigned transitions = 0;
  std::vector<Binding> bindings;
} state;
void ResetLocked(uint8_t* base, uint32_t manager) {
  if (state.base == base && state.manager == manager) return;
  state.base = base; state.manager = manager; ++state.epoch; state.bindings.clear();
}
std::optional<Token> Snapshot(uint8_t* base) {
  const auto manager = Read(base, kManager), settings = Read(base, kSettings);
  if (!manager || !settings) return {};
  const auto type = Read(base, settings + 4);
  if ((type != 3 && !(kTu4 && type == 20)) || Read(base, manager + 8 + 44) != type) return {};
  std::lock_guard lock(state.mutex);
  ResetLocked(base, manager);
  if (state.transitions) return {};
  return Token{base, manager, state.epoch};
}
bool Same(const Token& a, const Token& b) { return a.base == b.base && a.manager == b.manager && a.epoch == b.epoch; }
bool Current(const Token& token) { const auto now = Snapshot(token.base); return now && Same(*now, token); }
struct Transition {
  bool active = false;
  Transition(uint8_t* base, uint32_t destination) {
    const auto manager = Read(base, kManager);
    if (!manager || destination != manager + 8) return;
    std::lock_guard lock(state.mutex); ResetLocked(base, manager);
    ++state.transitions; ++state.epoch; state.bindings.clear(); active = true;
  }
  ~Transition() {
    if (!active) return;
    std::lock_guard lock(state.mutex); --state.transitions; ++state.epoch; state.bindings.clear();
  }
};
template<class T> struct Scope {
  T*& slot; T* previous;
  Scope(T*& target, T* value) : slot(target), previous(target) { slot = value; }
  Scope(T*& target, T& value) : Scope(target, &value) {}
  ~Scope() { slot = previous; }
};
struct Query { Token token; uint32_t receiver; bool seen = false; uint32_t record = 0; std::string full_key; };
thread_local Query* query = nullptr;
void Bind(const Query& q) {
  if (!q.record || q.full_key.empty() || !Current(q.token)) return;
  const auto actual = String(q.token.base, q.record + 4);
  if (!actual || *actual != q.full_key) return;
  std::lock_guard lock(state.mutex);
  if (state.transitions || state.epoch != q.token.epoch || state.base != q.token.base || state.manager != q.token.manager) return;
  std::erase_if(state.bindings, [&](const Binding& b) { return b.record == q.record; });
  if (state.bindings.size() < 32) state.bindings.push_back({q.token, q.record, q.full_key});
}
std::optional<Binding> Active(uint8_t* base, uint32_t record) {
  const auto token = Snapshot(base);
  if (!token || !record || Read(base, record + 80) != 3 || Read(base, record + 84) != 3) return {};
  const auto timeline = Read(base, record + 192);
  if (!timeline || Read(base, timeline + 4) != record) return {};
  const auto key = String(base, record + 4);
  if (!key) return {};
  std::lock_guard lock(state.mutex);
  if (state.transitions || state.epoch != token->epoch) return {};
  for (const auto& binding : state.bindings)
    if (Same(binding.token, *token) && binding.record == record && binding.full_key == *key) return binding;
  return {};
}
struct Request { Binding binding; uint32_t player; uint32_t audio_manager; bool redirected = false; };
thread_local Request* music = nullptr;
thread_local Request* request = nullptr;
bool Valid(const Request& r) {
  const auto active = Active(r.binding.token.base, r.binding.record);
  return active && Same(active->token, r.binding.token) && active->full_key == r.binding.full_key &&
         Read(r.binding.token.base, kPlayer) == r.player;
}
}

extern "C" REX_FUNC(EventRequestCopy) {
  if (!Enabled()) { OriginalEventRequestCopy(ctx, base); return; }
  Transition transition(base, ctx.r3.u32);
  OriginalEventRequestCopy(ctx, base);
}
extern "C" REX_FUNC(RaceCutsceneLookup) {
  const auto caller = ctx.lr;
  const auto receiver = ctx.r3.u32;
  const auto token = Enabled() && caller == kPrepareReturn ? Snapshot(base) : std::nullopt;
  const auto name = token ? String(base, ctx.r4.u32) : std::nullopt;
  if (!token || !name || !Canonical(*name)) { Scope mask(query, static_cast<Query*>(nullptr)); OriginalRaceCutsceneLookup(ctx, base); return; }
  Query q{*token, receiver, false, 0, {}};
  Scope scope(query, q);
  OriginalRaceCutsceneLookup(ctx, base);
  // The native first lookup owns the full key; fallbacks never confer authority.
  if (receiver && q.record && ctx.r3.u32 == q.record) Bind(q);
}
extern "C" REX_FUNC(CutsceneLookup) {
  const auto caller = ctx.lr;
  auto* q = query;
  const bool first = Enabled() && q && caller == kQualifiedReturn && !q->seen &&
                     q->token.base == base && ctx.r3.u32 == q->receiver && Current(q->token);
  const auto key = first ? String(base, ctx.r4.u32) : std::nullopt;
  if (first) q->seen = true;
  OriginalCutsceneLookup(ctx, base);
  if (first && key && Current(q->token)) { q->record = ctx.r3.u32; q->full_key = *key; }
}
extern "C" REX_FUNC(CutsceneMusicTrigger) {
  const auto binding = Enabled() ? Active(base, ctx.r3.u32) : std::nullopt;
  Scope mask_request(request, static_cast<Request*>(nullptr));
  if (!binding) { Scope mask(music, static_cast<Request*>(nullptr)); OriginalCutsceneMusicTrigger(ctx, base); return; }
  Request r{*binding, Read(base, kPlayer), Read(base, kAudioManager)};
  Scope scope(music, r);
  OriginalCutsceneMusicTrigger(ctx, base);
}
extern "C" REX_FUNC(StartEventMusic) {
  auto* r = music;
  if (!Enabled() || !r || r->binding.token.base != base || ctx.lr != kMusicReturn ||
      ctx.r3.u32 != r->player || !Valid(*r)) { Scope mask(request, static_cast<Request*>(nullptr)); OriginalStartEventMusic(ctx, base); return; }
  Scope scope(request, *r);
  OriginalStartEventMusic(ctx, base);
}
extern "C" REX_FUNC(QueueEventMusic) {
  auto* r = request;
  if (!Enabled() || !r || r->binding.token.base != base || r->redirected || ctx.lr != kQueueReturn || !Valid(*r)) {
    OriginalQueueEventMusic(ctx, base); return;
  }
  const auto manager = ctx.r3.u32, name = ctx.r4.u32;
  const auto table = manager && manager == r->audio_manager && manager == Read(base, kAudioManager) ? Read(base, manager) : 0;
  if (!table || Read(base, table + kFreeRoamSlot) != kFreeRoamQueue) { OriginalQueueEventMusic(ctx, base); return; }
  PPCContext call = ctx;
  call.f1.f64 = 1.0; // Native play_freeroam_music_with_fades; avoids cached zero-fade skip.
  call.r5.u64 = 0; call.r6.u64 = 0;
#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4
  PPCContext flag = ctx;
  FreeRoamFlag(flag, base);
  call.r6.u64 = (flag.r3.u32 & 0xFF) != 0;
#endif
  // The native predicate must not let a reentrant load commit stale authority.
  if (!Valid(*r) || Read(base, kAudioManager) != manager || Read(base, manager) != table ||
      Read(base, table + kFreeRoamSlot) != kFreeRoamQueue) { OriginalQueueEventMusic(ctx, base); return; }
  r->redirected = true; // Queue commit: matching marker must remain coherent across later load transitions.
  QueueFreeRoam(call, base);
  ctx.r3.u64 = name; ctx.r4.u64 = 1; ctx.r5.u64 = 0;
  DestroyOwnedString(ctx, base); // Replaced by-value queue argument, exactly once.
  REXLOG_INFO("Radio: retained selected station for a qualified career event cue");
}
extern "C" REX_FUNC(MarkScriptedStation) {
  auto* r = request;
  if (Enabled() && r && r->redirected && r->binding.token.base == base && ctx.r3.u32 == r->player && ctx.lr == kMarkerReturn) {
    ctx.r4.u64 = 0; ctx.r5.u64 = uint32_t(-1);
  }
  OriginalMarkScriptedStation(ctx, base);
}
