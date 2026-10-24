#pragma once

#include "car_tuning_model.h"
#include <cstdint>

struct PPCContext;

namespace forza::car_tuning {

// The owning native screen must establish these facts from its active game
// context before each call. Defaults deliberately deny access. Borrowed cars,
// races, multiplayer, loading and a detached/destroyed parent are unsupported.
struct Access {
  bool paused = false;
  bool offline_free_roam = false;
  bool owns_current_car = false;
  bool Allowed() const { return paused && offline_free_roam && owns_current_car; }
};

enum class NativeResult {
  Ready, Disabled, UnsupportedBuild, Unavailable, InvalidData,
  Unchanged, StaleSession, AllocationFailed, ApplyUnconfirmed, Applied
};

// Call only on the guest UI thread with its current PPC context and memory.
// Begin reads owned-car data; edits remain host-local until Apply. Apply uses
// FH1's native tuning model, physics refresh and profile save request. Applied
// confirms garage, live setup and cold-pressure readback, not a separately
// verified disk reload. The original native form remains opt-in until qualified.
bool NativeEnabled();
NativeResult BeginNative(const PPCContext&, uint8_t* base, Access, EditSession&);
// Capture all supported local fields using the loaded car's native gates,
// endpoints and stock defaults. Unit presentation remains a separate concern.
NativeResult BeginCatalogNative(const PPCContext&, uint8_t* base, Access, EditSession&);
NativeResult ApplyNative(const PPCContext&, uint8_t* base, Access, EditSession&);

// FH1's standalone performance evaluator owns a cloned definition and physics
// model. It never invokes the garage/live Apply operation. Release on the guest
// UI thread before discarding this handle, including on screen destruction.
struct PerformanceMetric { float value = 0; uint32_t status = 0; };
struct NativePreview {
  uint32_t model = 0, ticks = 0;
  Identity identity{};
  Payload original{}, live_original{}, definition_original{};
  std::array<PerformanceMetric, 7> metrics{};
  bool complete = false, failed = false, draft = false;
};
bool BeginPreviewNative(const PPCContext&, uint8_t*, Access, const EditSession&, NativePreview&);
bool AdvancePreviewNative(const PPCContext&, uint8_t*, Access, NativePreview&);
void ReleasePreviewNative(const PPCContext&, uint8_t*, NativePreview&);

} // namespace forza::car_tuning
