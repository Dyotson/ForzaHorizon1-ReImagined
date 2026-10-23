// The racing wheel driver with pedals and a shifter on their own USB devices (SDK patch 0128,
// #272), on Linux uinput devices through SDL: a wheel base ("SIMAGIC EVO PRO"), Simagic P1000
// pedals (0483:0525) and a Simagic DS-8X shifter (0483:0531).
//  - only the base is a wheel; the pedals drive the triggers and the clutch, the shifter's gear
//    buttons the gear, and the base's own pedal-like axis does nothing while the pedals are there;
//  - the pedals unplugged at run time: nothing from them, the wheel's own axes count again;
//    plugged back in, they work again; wheel_pedal_device = none leaves them alone.
// #303 (patch 0132):
//  - the H-pattern gear is asked of the game when the lever moves: a lever already in 3rd when the
//    driver starts asks for nothing (thumb_rx 10, the game's "no request"), a move asks for the new
//    gear for a moment, then nothing; wheel_shifter_follow_slot reports the slot all the time;
//
// Linux only (needs write access to /dev/uinput). Build against the patched SDK source and link
// the patched runtime, from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; clang++ -std=c++23 -msse4.1 -I$SDK/include
//       -I$SDK/thirdparty/sdl3/include -I$SDK/thirdparty/fmt/include -I$SDK/thirdparty/spdlog/include
//       -I$SDK/thirdparty/simde -I$SDK/src/input/wheel
//       -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL forza-recomp/tests/wheel_devices_probe.cpp
//       -L ~/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -lrexruntime -pthread
//       -Wl,-rpath,$HOME/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -o wheel-devices-probe
//   ./wheel-devices-probe
#include <rex/cvar.h>
#include <rex/input/wheel/wheel_ff.h>
#include <rex/input/wheel/wheel_input_driver.h>
#include <wheel_gear_request.h>

#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rex::input::wheel;
using rex::X_STATUS;

namespace {

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// A uinput device: axes (code, min, max, rest) and buttons BTN_TRIGGER + n (SDL's button n).
struct Device {
  int fd = -1;

  struct Axis {
    int code, min, max, rest;
  };
  bool Create(const char* name, uint16_t vid, uint16_t pid, std::vector<Axis> axes, int buttons) {
    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      return false;
    }
    if (buttons) {
      ioctl(fd, UI_SET_EVBIT, EV_KEY);
      for (int b = 0; b < buttons; b++) {
        ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER + b);
      }
    }
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (const auto& a : axes) {
      uinput_abs_setup abs{};
      abs.code = static_cast<__u16>(a.code);
      abs.absinfo.minimum = a.min;
      abs.absinfo.maximum = a.max;
      abs.absinfo.value = a.rest;
      ioctl(fd, UI_SET_ABSBIT, a.code);
      ioctl(fd, UI_ABS_SETUP, &abs);
    }
    uinput_setup setup{};
    setup.id = {BUS_USB, vid, pid, 0x111};
    std::snprintf(setup.name, sizeof(setup.name), "%s", name);
    return ioctl(fd, UI_DEV_SETUP, &setup) >= 0 && ioctl(fd, UI_DEV_CREATE) >= 0;
  }

  void Emit(int type, int code, int value) {
    input_event ev{};
    ev.type = static_cast<__u16>(type);
    ev.code = static_cast<__u16>(code);
    ev.value = value;
    write(fd, &ev, sizeof(ev));
    input_event syn{};
    syn.type = EV_SYN;
    syn.code = SYN_REPORT;
    write(fd, &syn, sizeof(syn));
  }
  void SetAxis(int code, int value) { Emit(EV_ABS, code, value); }
  void SetButton(int n, bool down) { Emit(EV_KEY, BTN_TRIGGER + n, down ? 1 : 0); }

  void Destroy() {
    if (fd >= 0) {
      ioctl(fd, UI_DEV_DESTROY);
      close(fd);
      fd = -1;
    }
  }
  ~Device() { Destroy(); }
};

void SetCvar(const char* name, const char* value) {
  for (rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
    if (entry.name == name) {
      entry.setter(value);
      return;
    }
  }
  Check(false, "cvar not found");
}

void Sleep(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The wheel's state as the game reads it.
rex::input::X_INPUT_GAMEPAD State(WheelInputDriver& d, rex::input::DeviceId id) {
  rex::input::X_INPUT_STATE s{};
  Check(d.GetDeviceState(id, &s) == 0, "state");
  return s.gamepad;
}

// Enumerates until `want` wheels are there (the driver rescans every 500 ms).
std::vector<rex::input::DeviceInfo> Wheels(WheelInputDriver& d, size_t want, int ms = 3000) {
  std::vector<rex::input::DeviceInfo> found;
  for (int t = 0; t < ms; t += 100) {
    found.clear();
    d.EnumerateDevices(found);
    if (found.size() == want) {
      break;
    }
    Sleep(100);
  }
  return found;
}

// Polls the state until `ok` holds (a rescan or an SDL update away).
template <typename Ok>
bool Until(WheelInputDriver& d, rex::input::DeviceId id, Ok ok, int ms = 2000) {
  for (int t = 0; t < ms; t += 50) {
    std::vector<rex::input::DeviceInfo> found;
    d.EnumerateDevices(found);
    if (ok(State(d, id))) {
      return true;
    }
    Sleep(50);
  }
  return false;
}

// Polls for `ms` (the game reads the wheel every frame): rescans happen, pedals at rest arm.
void Poll(WheelInputDriver& d, rex::input::DeviceId id, int ms) {
  for (int t = 0; t < ms; t += 50) {
    std::vector<rex::input::DeviceInfo> found;
    d.EnumerateDevices(found);
    State(d, id);
    Sleep(50);
  }
}

bool CreateBase(Device& base) {
  // Steering on X; Y rests at the minimum like a pedal input of the base, Z and Rz are centred.
  return base.Create("SIMAGIC EVO PRO", 0x3670, 0x0502,
                     {{ABS_X, -32768, 32767, 0}, {ABS_Y, 0, 65535, 0}, {ABS_Z, -32768, 32767, 0},
                      {ABS_RZ, -32768, 32767, 0}},
                     16);
}
bool CreatePedals(Device& pedals) {
  // P1000: Rx clutch, Ry brake, Rz throttle, all resting at the minimum.
  return pedals.Create("SIMAGIC P1000", 0x0483, 0x0525,
                       {{ABS_RX, 0, 65535, 0}, {ABS_RY, 0, 65535, 0}, {ABS_RZ, 0, 65535, 0}}, 0);
}

}  // namespace

int main() {
  // Only the virtual devices: SDL3 leaves out every other joystick with this hint.
  setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x3670/0x0502,0x0483/0x0525,0x0483/0x0531", 1);
  // SDL finds a plugged-in device by watching /dev/input, so the replug below also works where no
  // udev monitor reaches the process (a sandbox, a container).
  setenv("SDL_JOYSTICK_DISABLE_UDEV", "1", 1);
  SetCvar("wheel_shifter", "h");
  Device base, shifter;
  auto pedals = std::make_unique<Device>();
  Check(CreateBase(base), "create the uinput wheel base (needs /dev/uinput)");
  Check(CreatePedals(*pedals), "create the pedals");
  Check(shifter.Create("SIMAGIC DS-8X", 0x0483, 0x0531, {{ABS_X, -32768, 32767, 0}}, 11),
        "create the shifter");
  Sleep(500);  // udev

  std::printf("pedals and shifter on their own USB devices:\n");
  {
    WheelInputDriver driver(nullptr, 0);
    Check(driver.Setup() == X_STATUS_SUCCESS, "driver setup");
    auto found = Wheels(driver, 1);
    Check(found.size() == 1 && found[0].name == "SIMAGIC EVO PRO",
          "one wheel: the base; the pedals and the shifter are not wheels of their own");
    const auto id = found[0].id;
    rex::input::X_INPUT_CAPABILITIES caps{};
    Check(driver.GetDeviceCapabilities(id, 1, &caps) == 0 && (caps.gamepad.thumb_rx >> 8) == 7,
          "the capabilities report the DS-8X's seven gears");
    // Throttle (Rz), brake (Ry) and clutch (Rx) on the pedal device.
    Check(Until(driver, id, [](const auto& g) { return g.right_trigger == 0 && g.left_trigger == 0; }),
          "released pedals");
    pedals->SetAxis(ABS_RZ, 65535);
    Check(Until(driver, id, [](const auto& g) { return g.right_trigger == 255; }),
          "the pedal device's throttle");
    pedals->SetAxis(ABS_RZ, 0);
    pedals->SetAxis(ABS_RY, 32768);
    Check(Until(driver, id, [](const auto& g) { return g.left_trigger > 120 && g.left_trigger < 135 && g.right_trigger == 0; }),
          "the pedal device's brake, half way");
    pedals->SetAxis(ABS_RY, 0);
    pedals->SetAxis(ABS_RX, 65535);
    Check(Until(driver, id, [](const auto& g) { return g.thumb_ry > 32000; }),
          "the pedal device's clutch");
    pedals->SetAxis(ABS_RX, 0);
    // The base's Y rests at an end like a pedal: with the pedal device it is no pedal.
    base.SetAxis(ABS_Y, 65535);
    Sleep(100);
    Check(State(driver, id).right_trigger == 0, "the base's own axis is not the throttle");
    base.SetAxis(ABS_Y, 0);
    // Gears on the shifter device: button 0 is first gear, 8 reverse; none is neutral.
    shifter.SetButton(0, true);
    Check(Until(driver, id, [](const auto& g) { return g.thumb_rx == 1; }), "gear 1 engages");
    std::printf("  pedal device drives the triggers and the clutch; shifter device: gear 1\n");
    shifter.SetButton(0, false);
    shifter.SetButton(8, true);
    Check(Until(driver, id, [](const auto& g) { return g.thumb_rx == -1; }), "reverse");
    shifter.SetButton(8, false);
    Check(Until(driver, id, [](const auto& g) { return g.thumb_rx == 0; }), "neutral");
    // A shifter button presses no pad button, the base's buttons all work.
    shifter.SetButton(2, true);
    Check(Until(driver, id, [](const auto& g) { return g.thumb_rx == 3 && g.buttons == 0; }),
          "gear 3 presses no pad button");
    shifter.SetButton(2, false);

    // Unplugged: the pedals give nothing, the wheel's own detected pedal (Y) counts again once
    // it was seen at rest (as any pedal).
    pedals->Destroy();
    pedals = std::make_unique<Device>();
    Poll(driver, id, 800);
    base.SetAxis(ABS_Y, 65535);
    Check(Until(driver, id, [](const auto& g) { return g.right_trigger == 255; }, 3000),
          "unplugged: the base's own axis is the throttle again");
    base.SetAxis(ABS_Y, 0);
    Check(Until(driver, id, [](const auto& g) { return g.right_trigger == 0; }), "released");
    std::printf("  pedal device unplugged: the wheel's own pedal axes count\n");
    // Plugged back in: read again.
    Check(CreatePedals(*pedals), "replug the pedals");
    Sleep(500);  // udev
    Poll(driver, id, 800);
    pedals->SetAxis(ABS_RZ, 65535);
    Check(Until(driver, id, [](const auto& g) { return g.right_trigger == 255; }),
          "replugged: the pedal device's throttle again");
    pedals->SetAxis(ABS_RZ, 0);
    base.SetAxis(ABS_Y, 65535);
    Sleep(150);
    Check(State(driver, id).right_trigger == 0, "replugged: the base's axis is no pedal again");
    base.SetAxis(ABS_Y, 0);
    std::printf("  pedal device plugged back in: read again\n");
  }
  {
    // wheel_pedal_device = none: the pedal device is left alone (and is still no wheel).
    SetCvar("wheel_pedal_device", "none");
    WheelInputDriver driver(nullptr, 0);
    Check(driver.Setup() == X_STATUS_SUCCESS, "driver setup (none)");
    auto found = Wheels(driver, 1);
    Check(found.size() == 1, "none: still one wheel");
    const auto id = found[0].id;
    Sleep(100);
    pedals->SetAxis(ABS_RZ, 65535);
    Sleep(150);
    Check(State(driver, id).right_trigger == 0, "none: the pedal device does nothing");
    pedals->SetAxis(ABS_RZ, 0);
    SetCvar("wheel_pedal_device", "");
    std::printf("  wheel_pedal_device = none: the pedal device is not read\n");
  }

  {
    // #303: the H-pattern gear on lever moves. The lever is in 3rd when the driver starts.
    shifter.SetButton(2, true);
    Sleep(100);
    WheelInputDriver driver(nullptr, 0);
    Check(driver.Setup() == X_STATUS_SUCCESS, "driver setup (gear requests)");
    auto found = Wheels(driver, 1);
    Check(found.size() == 1, "gear requests: one wheel");
    const auto id = found[0].id;
    rex::input::X_INPUT_CAPABILITIES caps{};
    Check(driver.GetDeviceCapabilities(id, 1, &caps) == 0, "caps");
    Poll(driver, id, 800);  // the shifter device attached and read
    bool asked = false;
    for (int t = 0; t < 1500; t += 20) {
      asked = asked || State(driver, id).thumb_rx != kGearNoRequest;
      Sleep(20);
    }
    Check(!asked, "a lever left in 3rd asks for nothing (the game keeps its own gear at a start)");
    shifter.SetButton(2, false);
    shifter.SetButton(3, true);
    int reads_4 = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (State(driver, id).thumb_rx != 4 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
      Sleep(10);
    }
    t0 = std::chrono::steady_clock::now();
    while (State(driver, id).thumb_rx == 4) {
      reads_4++;
      Sleep(10);
    }
    const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
    Check(reads_4 > 3 && held >= 200 && held < 1000,
          "the lever moved into 4th: 4th asked for a moment");
    Check(State(driver, id).thumb_rx == kGearNoRequest, "then nothing: the paddles keep their gear");
    std::printf("  lever left in 3rd: no request; moved into 4th: 4th asked for %lld ms, then none\n",
                static_cast<long long>(held));
    // wheel_shifter_follow_slot: the slot all the time, as before.
    SetCvar("wheel_shifter_follow_slot", "true");
    Check(Until(driver, id, [](const auto& g) { return g.thumb_rx == 4; }), "follow: 4th");
    Poll(driver, id, 600);
    Check(State(driver, id).thumb_rx == 4, "follow: still 4th");
    SetCvar("wheel_shifter_follow_slot", "false");
    Check(Until(driver, id, [](const auto& g) { return g.thumb_rx == kGearNoRequest; }),
          "back to lever moves: nothing until the lever moves");
    shifter.SetButton(3, false);
    Check(Until(driver, id, [](const auto& g) { return g.thumb_rx == 0; }), "out to neutral: asked");
    std::printf("  wheel_shifter_follow_slot: the slot all the time\n");
  }
  std::printf("wheel devices probe: all checks passed\n");
  return 0;
}
