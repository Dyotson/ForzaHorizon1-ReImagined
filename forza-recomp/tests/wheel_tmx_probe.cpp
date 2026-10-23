// A Thrustmaster TMX as #308 saw it (SDK patch 0135), on a Linux uinput device through SDL:
// 044f:b67f named "44F B67F" (its Windows driver gives it no name), four axes (X, Y, Rz, Slider)
// that all read 0 until its first report, 13 buttons.
//  - the driver reads where the axes rest at the first report, and the built-in TMX map puts the
//    steering on X, the throttle on Rz, the brake on Y and the clutch on the slider, the paddles
//    on the bumpers and A on button 4;
//
// Linux only (needs write access to /dev/uinput). Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only) and link the patched runtime, from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; clang++ -std=c++23 -msse4.1 -I$SDK/include
//       -I$SDK/thirdparty/sdl3/include -I$SDK/thirdparty/fmt/include -I$SDK/thirdparty/spdlog/include
//       -I$SDK/thirdparty/simde
//       -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL forza-recomp/tests/wheel_tmx_probe.cpp
//       -L ~/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -lrexruntime -pthread
//       -Wl,-rpath,$HOME/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -o wheel-tmx-probe
//   ./wheel-tmx-probe
#include <rex/cvar.h>
#include <rex/input/wheel/wheel_ff.h>
#include <rex/input/wheel/wheel_input_driver.h>

#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

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

// The TMX: X, Y, Rz and Slider (ABS_THROTTLE on Linux) all at 0, 13 buttons, force feedback that
// accepts a spring and refuses a constant force; counts the uploads of each.
struct VirtualTmx {
  int fd = -1;
  std::atomic<bool> stop{false};
  std::atomic<int> constant_tries{0}, spring_uploads{0};
  std::thread reader;

  bool Create() {
    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      return false;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int b = 0; b < 13; b++) {
      ioctl(fd, UI_SET_KEYBIT, BTN_TRIGGER + b);
    }
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int code : {ABS_X, ABS_Y, ABS_RZ, ABS_THROTTLE}) {
      uinput_abs_setup abs{};
      abs.code = static_cast<__u16>(code);
      abs.absinfo.minimum = -32768;
      abs.absinfo.maximum = 32767;
      abs.absinfo.value = 0;  // no report yet
      ioctl(fd, UI_SET_ABSBIT, code);
      ioctl(fd, UI_ABS_SETUP, &abs);
    }
    ioctl(fd, UI_SET_EVBIT, EV_FF);
    for (int f : {FF_CONSTANT, FF_SPRING, FF_GAIN}) {
      ioctl(fd, UI_SET_FFBIT, f);
    }
    uinput_setup setup{};
    setup.id = {BUS_USB, 0x044F, 0xB67F, 0x111};
    std::snprintf(setup.name, sizeof(setup.name), "%s", "44F B67F");
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
          up.retval = 0;
          if (up.effect.type == FF_CONSTANT) {
            constant_tries++;
            up.retval = -EINVAL;  // "Unable to create effect"
          } else if (up.effect.type == FF_SPRING) {
            spring_uploads++;
          }
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

  ~VirtualTmx() {
    stop = true;
    if (reader.joinable()) {
      reader.join();
    }
    if (fd >= 0) {
      ioctl(fd, UI_DEV_DESTROY);
      close(fd);
    }
  }
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

rex::input::X_INPUT_GAMEPAD State(WheelInputDriver& d, rex::input::DeviceId id) {
  rex::input::X_INPUT_STATE s{};
  Check(d.GetDeviceState(id, &s) == 0, "state");
  return s.gamepad;
}

template <typename Ok>
bool Until(WheelInputDriver& d, rex::input::DeviceId id, Ok ok, int ms = 2000) {
  for (int t = 0; t < ms; t += 20) {
    if (ok(State(d, id))) {
      return true;
    }
    Sleep(20);
  }
  return false;
}

FfEffect Effect(uint32_t block, uint32_t type, int magnitude) {
  FfEffect e;
  e.block = block;
  e.type = type;
  e.gain = 255;
  e.magnitude = magnitude;
  if (type == kFfSpring) {
    e.pos_coeff = e.neg_coeff = 63;
    e.pos_sat = e.neg_sat = 127;
  }
  return e;
}

}  // namespace

int main() {
  setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x044f/0xb67f", 1);
  setenv("SDL_JOYSTICK_DISABLE_UDEV", "1", 1);
  ForzaWheelPlayerDriving(1);
  VirtualTmx tmx;
  Check(tmx.Create(), "create the uinput TMX (needs /dev/uinput)");
  Sleep(500);  // udev
  WheelInputDriver driver(nullptr, 0);
  Check(driver.Setup() == X_STATUS_SUCCESS, "driver setup");
  std::vector<rex::input::DeviceInfo> found;
  for (int t = 0; t < 3000 && found.empty(); t += 100) {
    driver.EnumerateDevices(found);
    if (found.empty()) {
      Sleep(100);
    }
  }
  Check(found.size() == 1, "the TMX is a wheel (by its id: its name says nothing)");
  const auto id = found[0].id;
  rex::input::X_INPUT_CAPABILITIES caps{};
  Check(driver.GetDeviceCapabilities(id, 1, &caps) == 0, "caps");
  std::printf("the TMX, every axis at 0 until its first report:\n");
  // Nothing before the first report.
  for (int i = 0; i < 10; i++) {
    const auto g = State(driver, id);
    Check(g.right_trigger == 0 && g.left_trigger == 0 && g.thumb_lx == 0, "no input yet");
    Sleep(10);
  }
  // The first report: the pedals rest at the maximum, the wheel in the middle.
  tmx.SetAxis(ABS_Y, 32767);
  tmx.SetAxis(ABS_RZ, 32767);
  tmx.SetAxis(ABS_THROTTLE, 32767);
  Check(Until(driver, id, [](const auto& g) { return g.right_trigger == 0 && g.left_trigger == 0; }),
        "released at the first report");
  Sleep(100);
  tmx.SetAxis(ABS_RZ, -32768);
  Check(Until(driver, id, [](const auto& g) { return g.right_trigger == 255 && g.left_trigger == 0; }),
        "Rz is the throttle");
  tmx.SetAxis(ABS_RZ, 32767);
  tmx.SetAxis(ABS_Y, 0);
  Check(Until(driver, id, [](const auto& g) { return g.left_trigger > 120 && g.left_trigger < 135; }),
        "Y is the brake, half way");
  tmx.SetAxis(ABS_Y, 32767);
  tmx.SetAxis(ABS_THROTTLE, -32768);
  Check(Until(driver, id, [](const auto& g) { return g.thumb_ry > 32000; }), "the slider is the clutch");
  tmx.SetAxis(ABS_THROTTLE, 32767);
  tmx.SetAxis(ABS_X, 8192);
  Check(Until(driver, id, [](const auto& g) { return g.thumb_lx > 8000 && g.thumb_lx < 8400; }),
        "X is the steering, from the centre");
  tmx.SetAxis(ABS_X, 0);
  std::printf("  steering X, throttle Rz, brake Y, clutch slider; rest read at the first report\n");
  // Buttons: the paddles (0 left, 1 right) are LB and RB, A is button 4, B 8.
  tmx.SetButton(1, true);
  Check(Until(driver, id, [](const auto& g) { return g.buttons == 0x0200; }), "right paddle: RB");
  tmx.SetButton(1, false);
  tmx.SetButton(0, true);
  Check(Until(driver, id, [](const auto& g) { return g.buttons == 0x0100; }), "left paddle: LB");
  tmx.SetButton(0, false);
  tmx.SetButton(4, true);
  Check(Until(driver, id, [](const auto& g) { return g.buttons == 0x1000; }), "button 4: A");
  tmx.SetButton(4, false);
  tmx.SetButton(8, true);
  Check(Until(driver, id, [](const auto& g) { return g.buttons == 0x2000; }), "button 8: B");
  tmx.SetButton(8, false);
  std::printf("  the paddles shift (LB, RB), A on 4, B on 8\n");

  std::printf("wheel tmx probe: all checks passed\n");
  return 0;
}
