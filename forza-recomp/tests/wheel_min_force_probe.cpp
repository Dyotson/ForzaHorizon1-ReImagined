// The minimum steering force (SDK patch 0131, #303) on a real SDL haptic device: a Linux uinput
// wheel with a constant force and a spring (the Simagic base id of wheel_devices_probe.cpp). The
// probe plays the game's steering force through the kernel-facing calls (GameFf*) at several sizes
// and reads back the level each upload sends to the device, with wheel_ffb_min_strength 0 (as
// before) and 30: weak forces are scaled up, strong ones arrive as before, the sign stays, and the
// spring is not touched.
//
// Linux only (needs write access to /dev/uinput). Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only) and link the patched runtime, from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; clang++ -std=c++23 -msse4.1 -I$SDK/include
//       -I$SDK/thirdparty/sdl3/include -I$SDK/thirdparty/fmt/include -I$SDK/thirdparty/spdlog/include
//       -I$SDK/thirdparty/simde -I$SDK/src/input/wheel
//       -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL forza-recomp/tests/wheel_min_force_probe.cpp
//       -L ~/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -lrexruntime -pthread
//       -Wl,-rpath,$HOME/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -o wheel-min-force-probe
//   ./wheel-min-force-probe
#include <rex/cvar.h>
#include <rex/input/wheel/wheel_ff.h>
#include <rex/input/wheel/wheel_input_driver.h>
#include <wheel_min_force.h>

#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace rex::input::wheel;
using rex::X_STATUS;

extern "C" void ForzaWheelPlayerDriving(int driving);

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

FfEffect Constant(int magnitude) {
  FfEffect e;
  e.block = 5;
  e.type = kFfConstant;
  e.gain = 255;
  e.magnitude = magnitude;
  return e;
}

// FH1's spring: block 4, coefficients 63, saturations 127.
FfEffect Spring() {
  FfEffect e;
  e.block = 4;
  e.type = kFfSpring;
  e.gain = 255;
  e.pos_coeff = e.neg_coeff = 63;
  e.pos_sat = e.neg_sat = 127;
  return e;
}

// The level the wheel gets for the game's steering force `magnitude` (-127 to 127).
int Play(VirtualWheel& wheel, int magnitude) {
  const int before = wheel.constant_uploads;
  Check(GameFfUpdateEffect(Constant(magnitude), 0x400), "update the steering force");
  for (int i = 0; i < 100 && wheel.constant_uploads == before; i++) {
    Sleep(5);
  }
  Sleep(20);
  return wheel.constant;
}

}  // namespace

int main() {
  setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x3670/0x0502", 1);
  ForzaWheelPlayerDriving(1);  // driving: no fade
  SetCvar("wheel_ffb_strength", "100");
  VirtualWheel wheel;
  Check(wheel.Create(), "create the uinput wheel (needs /dev/uinput)");
  Sleep(500);  // udev
  WheelInputDriver driver(nullptr, 0);
  Check(driver.Setup() == X_STATUS_SUCCESS, "driver setup");
  std::vector<rex::input::DeviceInfo> found;
  for (int i = 0; i < 50 && found.empty(); i++) {
    driver.EnumerateDevices(found);
    if (found.empty()) {
      Sleep(100);
    }
  }
  Check(found.size() == 1, "the wheel");
  // The game's effects, as FH1 sets them up: spring (block 4) and the steering force (block 5).
  Check(GameFfControl(kFfStopAll), "stop all");
  Check(GameFfSetEffect(Spring()), "spring");
  Check(GameFfSetEffect(Constant(0)), "steering force");
  Check(GameFfOperate(4, 2) && GameFfOperate(5, 2), "start them");
  Sleep(100);
  const int spring_before = wheel.spring;

  std::printf("the game's steering force -> the level the wheel gets (strength 100):\n");
  std::printf("  game   min 0   min 30\n");
  const int sizes[] = {3, 13, 25, 38, 64, 127, -13, -64};
  int plain[8], boosted[8];
  SetCvar("wheel_ffb_min_strength", "0");
  for (int i = 0; i < 8; i++) {
    plain[i] = Play(wheel, sizes[i]);
  }
  SetCvar("wheel_ffb_min_strength", "30");
  for (int i = 0; i < 8; i++) {
    boosted[i] = Play(wheel, sizes[i]);
    std::printf("  %4d  %6d  %7d\n", sizes[i], plain[i], boosted[i]);
  }
  for (int i = 0; i < 8; i++) {
    const float game = std::fabs(sizes[i] / 127.0f);
    // Before: the game's level as it was (sign: FH1's positive force pulls right, SDL's left).
    Check(std::abs(plain[i] - static_cast<int>(std::lround(-sizes[i] / 127.0f * 32767.0f))) <= 1,
          "min 0: as before");
    Check((boosted[i] < 0) == (plain[i] < 0), "the direction stays");
    if (game < 0.3f) {
      Check(std::abs(boosted[i]) > std::abs(plain[i]), "a weak force is scaled up");
      Check(std::abs(std::abs(boosted[i]) - MinForce(game, 0.3f) * 32767.0f) <= 2.0f,
            "by the curve");
    } else {
      Check(boosted[i] == plain[i], "a strong force arrives as before");
    }
  }
  Check(Play(wheel, 0) == 0, "no force stays none");
  Check(wheel.spring == spring_before, "the spring is not touched");
  SetCvar("wheel_ffb_min_strength", "0");
  std::printf("wheel min force probe: all checks passed\n");
  return 0;
}
