// Virtual-controller probe. Private access is used only to register fixtures;
// assertions exercise public SDK output methods and real SDL callbacks.
#include <rex/input/input_system.h>
#include <rex/input/input_driver.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <optional>
#define private public
#include <rex/input/sdl/sdl_input_driver.h>
#undef private
#include <rex/cvar.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

struct Motors {
  Uint16 body_l = 0, body_r = 0, left = 0, right = 0;
  int body_calls = 0, trigger_calls = 0;
};
void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError());
    std::exit(1);
  }
}
bool SDLCALL Body(void* ptr, Uint16 left, Uint16 right) {
  auto& motors = *static_cast<Motors*>(ptr);
  motors.body_l = left;
  motors.body_r = right;
  ++motors.body_calls;
  return true;
}
bool SDLCALL Triggers(void* ptr, Uint16 left, Uint16 right) {
  auto& motors = *static_cast<Motors*>(ptr);
  motors.left = left;
  motors.right = right;
  ++motors.trigger_calls;
  return true;
}
SDL_Gamepad* Pad(Motors& motors, bool capable = true) {
  SDL_VirtualJoystickDesc desc{};
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.vendor_id = 0x045e;
  desc.product_id = 0x02d1;
  desc.naxes = 6;
  desc.nbuttons = 15;
  desc.axis_mask = 0x3f;
  desc.button_mask = 0x7fff;
  desc.name = "FH1 virtual Xbox test";
  desc.userdata = &motors;
  desc.Rumble = Body;
  if (capable) desc.RumbleTriggers = Triggers;
  const auto id = SDL_AttachVirtualJoystick(&desc);
  Check(id != 0, "attach virtual pad");
  auto* pad = SDL_OpenGamepad(id);
  Check(pad != nullptr, "open virtual pad");
  char* current = SDL_GetGamepadMapping(pad);
  Check(current != nullptr, "mapping");
  std::string mapping = current;
  SDL_free(current);
  mapping += ",type:xboxone,";
  Check(SDL_SetGamepadMapping(id, mapping.c_str()), "set Xbox type");
  Check(SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_XBOXONE, "Xbox classification");
  return pad;
}
void VibrationSetting(const char* value) {
  for (auto& entry : rex::cvar::GetRegistry())
    if (entry.name == "vibration") entry.setter(value);
}
int main() {
  Check(SDL_Init(SDL_INIT_GAMEPAD), "init SDL");
  Motors first, second, unsupported;
  auto* p1 = Pad(first);
  auto* p2 = Pad(second);
  auto* p3 = Pad(unsupported, false);
  auto driver = std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0);
  auto* raw = driver.get();
  raw->sdl_events_initialized_ = raw->SDL_Gamepad_initialized_ = true;
  int ordinal = 0;
  for (auto* pad : {p1, p2, p3}) {
    rex::input::sdl::SDLInputDriver::ControllerState state{};
    state.sdl = pad;
    state.id = static_cast<rex::input::DeviceId>(++ordinal);
    raw->controllers_.push_back(state);
  }
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::move(driver));
  input.SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  Check(SDL_RumbleGamepad(p1, 1234, 5678, 10000), "body rumble start");
  const int body_count = first.body_calls;
  Check(input.SetTriggerRumble(1, 10000, 20000, 150) == 1, "route to user zero");
  Check(first.left == 10000 && first.right == 20000, "trigger amplitudes");
  Check(first.body_l == 1234 && first.body_r == 5678 && first.body_calls == body_count,
        "preserve body rumble");
  Check(second.trigger_calls == 0, "do not route to user one");
  Check(input.SetTriggerRumble(2, 30000, 40000, 150) == 1 && second.left == 30000,
        "route to user one");
  Check(input.SetTriggerRumble(4, 30000, 40000, 150) == 0 && unsupported.trigger_calls == 0,
        "reject unsupported pad");
  Check(input.SetTriggerRumble(1, 10000, 20000, 5000) == 1, "start bounded effect");
  SDL_Delay(200);
  SDL_UpdateJoysticks();
  Check(first.left == 0 && first.right == 0, "native 150ms timeout");
  Check(first.body_l == 1234 && first.body_r == 5678 && first.body_calls == body_count,
        "timeout preserves body rumble");
  VibrationSetting("false");
  Check(input.SetTriggerRumble(1, 10000, 20000, 150) == 1 && first.left == 0,
        "master vibration toggle releases");
  VibrationSetting("true");
  raw->set_is_active_callback([] { return false; });
  Check(input.SetTriggerRumble(1, 5000, 5000, 150) == 1 && first.left == 0,
        "focus loss releases");
  raw->set_is_active_callback([] { return true; });
  input.AddUIInputBlocker();
  Check(input.SetTriggerRumble(1, 10000, 20000, 150) == 1 && first.left == 0 && first.right == 0,
        "UI blocker releases");
  input.Shutdown();
  SDL_CloseGamepad(p1);
  SDL_CloseGamepad(p2);
  SDL_CloseGamepad(p3);
  SDL_Quit();
  std::puts("PASS: SDL trigger amplitudes, user routing, capability, focus/UI/vibration gating, "
            "body preservation and timeout");
}
