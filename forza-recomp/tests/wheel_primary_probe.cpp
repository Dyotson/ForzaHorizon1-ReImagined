// Which joystick is the wheel (SDK patch 0134, #306) on Linux uinput devices through SDL: a
// virtual joystick like vJoy (1234:bead, "vJoy - Virtual Joystick", no force feedback) that SDL
// lists first, and a wheel base with force feedback (the Simagic id of wheel_devices_probe.cpp).
// With wheel_mode = on both are wheels:
//  - automatic: the base with force feedback is the first wheel (the first player's);
//  - wheel_device names the vJoy (by name or vid:pid): it is the first wheel;
//  - wheel_pedal_device names the vJoy (SimHub's pedals): it is no wheel at all;
//  - wheel_mode = auto: the vJoy is no wheel, unless wheel_device names it.
//
// Linux only (needs write access to /dev/uinput). Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only) and link the patched runtime, from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; clang++ -std=c++23 -msse4.1 -I$SDK/include
//       -I$SDK/thirdparty/sdl3/include -I$SDK/thirdparty/fmt/include -I$SDK/thirdparty/spdlog/include
//       -I$SDK/thirdparty/simde
//       -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL forza-recomp/tests/wheel_primary_probe.cpp
//       -L ~/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -lrexruntime -pthread
//       -Wl,-rpath,$HOME/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -o wheel-primary-probe
//   ./wheel-primary-probe
#include <rex/cvar.h>
#include <rex/input/wheel/wheel_input_driver.h>

#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

void Sleep(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// A uinput wheel with constant force, spring and gain; records the last constant level and spring
// coefficient uploaded.
struct VirtualWheel {
  int fd = -1;
  std::atomic<bool> stop{false};
  std::atomic<int> constant{0}, constant_uploads{0}, spring{-1};
  std::thread reader;

  bool Create() {
    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      return false;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int b = BTN_TRIGGER; b <= BTN_DEAD; b++) {
      ioctl(fd, UI_SET_KEYBIT, b);
    }
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int code : {ABS_X, ABS_Y, ABS_Z}) {
      uinput_abs_setup abs{};
      abs.code = static_cast<__u16>(code);
      abs.absinfo.minimum = code == ABS_X ? -32768 : 0;
      abs.absinfo.maximum = code == ABS_X ? 32767 : 65535;
      ioctl(fd, UI_SET_ABSBIT, code);
      ioctl(fd, UI_ABS_SETUP, &abs);
    }
    ioctl(fd, UI_SET_EVBIT, EV_FF);
    for (int f : {FF_CONSTANT, FF_SPRING, FF_GAIN}) {
      ioctl(fd, UI_SET_FFBIT, f);
    }
    uinput_setup setup{};
    setup.id = {BUS_USB, 0x3670, 0x0502, 0x111};
    std::snprintf(setup.name, sizeof(setup.name), "%s", "SIMAGIC EVO PRO");
    setup.ff_effects_max = 16;
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
      return false;
    }
    reader = std::thread([this] { Read(); });
    return true;
  }

  void Read() {
    while (!stop) {
      pollfd p{fd, POLLIN, 0};
      if (poll(&p, 1, 20) <= 0) {
        continue;
      }
      input_event ev{};
      while (read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
          uinput_ff_upload up{};
          up.request_id = static_cast<__u32>(ev.value);
          ioctl(fd, UI_BEGIN_FF_UPLOAD, &up);
          if (up.effect.type == FF_CONSTANT) {
            constant = up.effect.u.constant.level;
            constant_uploads++;
          } else if (up.effect.type == FF_SPRING) {
            spring = up.effect.u.condition[0].right_coeff;
          }
          up.retval = 0;
          ioctl(fd, UI_END_FF_UPLOAD, &up);
        } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
          uinput_ff_erase er{};
          er.request_id = static_cast<__u32>(ev.value);
          ioctl(fd, UI_BEGIN_FF_ERASE, &er);
          er.retval = 0;
          ioctl(fd, UI_END_FF_ERASE, &er);
        }
      }
    }
  }

  void Destroy() {
    stop = true;
    if (reader.joinable()) {
      reader.join();
    }
    if (fd >= 0) {
      ioctl(fd, UI_DEV_DESTROY);
      close(fd);
      fd = -1;
    }
  }
  ~VirtualWheel() { Destroy(); }
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

// A joystick without force feedback: X, Y, Z and eight buttons (udev takes it for a joystick).
struct VirtualJoystick {
  int fd = -1;
  bool Create(const char* name, uint16_t vid, uint16_t pid) {
    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      return false;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int b = 0; b < 8; b++) {
      ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER + b);
    }
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int code : {ABS_X, ABS_Y, ABS_Z}) {
      uinput_abs_setup abs{};
      abs.code = static_cast<__u16>(code);
      abs.absinfo.minimum = -32768;
      abs.absinfo.maximum = 32767;
      ioctl(fd, UI_SET_ABSBIT, code);
      ioctl(fd, UI_ABS_SETUP, &abs);
    }
    uinput_setup setup{};
    setup.id = {BUS_USB, vid, pid, 0x111};
    std::snprintf(setup.name, sizeof(setup.name), "%s", name);
    return ioctl(fd, UI_DEV_SETUP, &setup) >= 0 && ioctl(fd, UI_DEV_CREATE) >= 0;
  }
  ~VirtualJoystick() {
    if (fd >= 0) {
      ioctl(fd, UI_DEV_DESTROY);
      close(fd);
    }
  }
};

// The wheels a fresh driver presents, in its order (the first is the first player's).
std::vector<std::string> Wheels(size_t want) {
  WheelInputDriver driver(nullptr, 0);
  Check(driver.Setup() == X_STATUS_SUCCESS, "driver setup");
  std::vector<rex::input::DeviceInfo> found;
  for (int t = 0; t < 3000; t += 100) {
    found.clear();
    driver.EnumerateDevices(found);
    if (found.size() == want) {
      break;
    }
    Sleep(100);
  }
  std::vector<std::string> names;
  for (const auto& d : found) {
    names.push_back(d.name);
  }
  return names;
}

void Print(const char* what, const std::vector<std::string>& names) {
  std::printf("  %-44s", what);
  for (const auto& n : names) {
    std::printf(" [%s]", n.c_str());
  }
  std::printf("\n");
}

}  // namespace

int main() {
  setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x1234/0xbead,0x3670/0x0502", 1);
  setenv("SDL_JOYSTICK_DISABLE_UDEV", "1", 1);
  const std::string vjoy = "vJoy - Virtual Joystick", base = "SIMAGIC EVO PRO";
  VirtualJoystick joystick;  // created first: SDL lists it first
  Check(joystick.Create(vjoy.c_str(), 0x1234, 0xBEAD), "create the vJoy (needs /dev/uinput)");
  Sleep(300);
  VirtualWheel wheel;
  Check(wheel.Create(), "create the wheel base");
  Sleep(500);  // udev
  std::printf("two wheel-like devices, the vJoy first in SDL's order:\n");
  SetCvar("wheel_mode", "on");
  auto names = Wheels(2);
  Print("wheel_mode = on:", names);
  Check(names == std::vector<std::string>{base, vjoy}, "the base with force feedback is the wheel");
  SetCvar("wheel_device", vjoy.c_str());
  names = Wheels(2);
  Print("wheel_device = its name:", names);
  Check(names == std::vector<std::string>{vjoy, base}, "wheel_device picks the vJoy");
  SetCvar("wheel_device", "1234:bead");
  names = Wheels(2);
  Print("wheel_device = 1234:bead:", names);
  Check(names.size() == 2 && names[0] == vjoy, "by vid:pid");
  SetCvar("wheel_device", "");
  SetCvar("wheel_pedal_device", vjoy.c_str());
  names = Wheels(1);
  Print("wheel_pedal_device = the vJoy:", names);
  Check(names == std::vector<std::string>{base}, "the pedal device is no wheel");
  SetCvar("wheel_pedal_device", "");
  SetCvar("wheel_mode", "auto");
  names = Wheels(1);
  Print("wheel_mode = auto:", names);
  Check(names == std::vector<std::string>{base}, "auto: the vJoy is no wheel");
  SetCvar("wheel_device", "vjoy");
  names = Wheels(2);
  Print("wheel_mode = auto, wheel_device = vjoy:", names);
  Check(names == std::vector<std::string>{vjoy, base}, "named, it is a wheel, and the first");
  SetCvar("wheel_device", "");
  std::printf("wheel primary probe: all checks passed\n");
  return 0;
}
