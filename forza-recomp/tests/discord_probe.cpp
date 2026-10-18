#include "../src/platform/discord_model.h"
#include <discord_rpc.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace forza::discord;
int main(int argc, char** argv) {
  assert(!ValidApplicationId(""));
  assert(!ValidApplicationId("secret-token"));
  assert(!ValidApplicationId("123"));
  assert(!ValidApplicationId("18446744073709551616"));
  assert(ValidApplicationId("123456789012345678"));
  Model model;
  assert(model.Format().details == "At the main menu");
  assert(!model.Context(0x10000039, 500));  // online rank must not leak into the status
  model.Context(0x8001, 1);
  assert(model.Format().details == "Exploring Colorado");
  model.Context(0x8001, 2);
  assert(model.Format().details == "Racing in a festival event");
  model.Context(0x16, 1);
  assert(model.Format().details == "Racing in Horizon Rally");
  model.Context(0x8001, 14);
  model.count = 3; model.capacity = 8;
  assert(model.Format().state == "Multiplayer | 3/8 players");
  model.Context(0x8001, 15); model.Context(0x12, 4);
  assert(model.Format().details == "Exploring Colorado online");
  model.count = 99;
  assert(model.Format().count == 0);
  for (uint32_t mode = 0; mode <= 16; ++mode) {
    model.Context(0x8001, mode);
    assert(!model.Format().details.empty());
  }
  if (argc == 1) { std::puts("Discord model PASS"); return 0; }
  // A local Discord-compatible IPC fixture exercises the actual pinned client.
  DiscordEventHandlers handlers{};
  handlers.ready = [](const DiscordUser*) { std::puts("READY"); std::fflush(stdout); };
  handlers.errored = [](int, const char*) { std::puts("ERROR"); std::fflush(stdout); };
  Discord_Initialize(argv[1], &handlers, 0, nullptr);
  model.Context(0x8001, 1);
  auto activity = model.Format();
  DiscordRichPresence presence{};
  presence.details = activity.details.c_str(); presence.state = activity.state.c_str();
  presence.startTimestamp = 1000;
  Discord_UpdatePresence(&presence);
  for (int i = 0; i < 160; ++i) {
    Discord_RunCallbacks(); std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Discord_ClearPresence();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Discord_Shutdown();
  std::puts("Discord IPC PASS");
}
