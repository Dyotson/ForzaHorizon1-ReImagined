// forza - a car's picture and its livery are rendered with real pipelines (#74, #77,
// docs/rendering.md).
//
// The game renders a car's picture once (the purchase, Car Select, the garage), reads the
// resolve back on the CPU and saves it in the profile (ForzaProfile/Thumbnails). With
// async_shader_compilation the SDK skips a draw whose pipeline is still being created, so
// with an empty pipeline cache (a new install's first purchase) the car's draws were lost
// and the saved picture was nearly black (23-33 KB: only the narrow side view, no car).
//
// sub_82546460 starts the picture (it calls the save, sub_82546168, and returns before the
// GPU reaches the render: the command processor runs behind the guest, by 270 ms in a cold
// run). So the window is by time, not by the call: from the call the SDK setting
// sync_pipeline_creation (patch 0051) makes the command processor create or wait for the
// real pipeline of each draw, for kWindow. Other draws in that time wait for their pipeline
// too, as with async_shader_compilation=false; with a warm cache nothing waits.
// FORZA_THUMB_SYNC=0 switches the hook off.
//
// The livery (paint, vinyls and decals) is drawn once into the car's 1024x1024 textures when
// the car is loaded, and the same skipped draws left those textures without some of their
// layers for as long as the car stayed loaded (#77: with an empty cache the BMW M3 GTR at
// the festival lost the white of its left front wing). sub_82C8FE60 sets up each livery
// element render (render scenario "RenderLiveryElements" or "...Highlight"; called 10 times
// within 1 ms at the festival load), and holds the same window. FORZA_LIVERY_SYNC=0 switches
// that hook off.

#include "../platform/guest_addr.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

constexpr auto kWindow = std::chrono::seconds(8);

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return !(v && std::strcmp(v, "0") == 0);
}

void SetSync(bool on) {
  for (rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
    if (entry.name == "sync_pipeline_creation") {
      entry.setter(on ? "true" : "false");
      return;
    }
  }
}

std::atomic<uint64_t> g_generation{0};

// Sets the SDK setting now and clears it kWindow after the last call.
void HoldSync() {
  const uint64_t generation = g_generation.fetch_add(1) + 1;
  SetSync(true);
  std::thread([generation] {
    std::this_thread::sleep_for(kWindow);
    if (g_generation.load() == generation) SetSync(false);
  }).detach();
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_82546460);
extern "C" REX_FUNC(sub_82546460) {
  static const bool on = EnvOn("FORZA_THUMB_SYNC");
  if (on) HoldSync();
  __imp__sub_82546460(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82C8FE60);
extern "C" REX_FUNC(sub_82C8FE60) {
  static const bool on = EnvOn("FORZA_LIVERY_SYNC");
  if (on) HoldSync();
  __imp__sub_82C8FE60(ctx, base);
}
