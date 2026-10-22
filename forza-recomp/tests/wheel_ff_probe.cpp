// The racing wheel driver's game force feedback on a real SDL haptic device (SDK patch 0101): a
// Linux uinput wheel with only constant force and autocentre, like the hid-lg4ff driver of a G29,
// so the game's spring becomes the wheel's autocentre. The probe drives the actual WheelInputDriver
// through the kernel-facing calls (GameFf*) and reads back what reaches the device: the autocentre
// must follow the spring and drop to 0 when it stops, is reset or becomes another type (#219 review:
// a reset left the hardware autocentre at its last value). Then (#256) the forces fade out while
// the game says the player is not driving (ForzaWheelPlayerDriving) and come back when they are,
// and wheel_ffb_spring_strength scales the spring.
//
// Linux only (needs write access to /dev/uinput). Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only) and link the patched runtime, from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; clang++ -std=c++23 -msse4.1 -I$SDK/include
//       -I$SDK/thirdparty/sdl3/include -I$SDK/thirdparty/fmt/include -I$SDK/thirdparty/spdlog/include
//       -I$SDK/thirdparty/simde
//       -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL forza-recomp/tests/wheel_ff_probe.cpp
//       -L ~/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -lrexruntime -pthread
//       -Wl,-rpath,$HOME/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64/lib -o wheel-ff-probe
//   ./wheel-ff-probe
#include <rex/cvar.h>
#include <rex/input/wheel/wheel_ff.h>
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
#include <cstring>
#include <thread>
#include <vector>

using namespace rex::input::wheel;
using rex::X_STATUS;

extern "C" void ForzaWheelPlayerDriving(int driving);  // the runtime's export (#256)

namespace {

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

// A uinput G29 (046d:c24f) with constant force, gain and autocentre. It accepts every effect
// upload and records the last autocentre the driver set (0-0xFFFF).
struct VirtualWheel {
  int fd = -1;
  std::atomic<bool> stop{false};
  std::atomic<int> autocenter{-1};
  std::atomic<int> autocenter_writes{0};
  std::atomic<int> autocenter_between{0};  // writes strictly between 0 and full (a fade's steps)
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
    const struct {
      int code, min, max, value;
    } axes[] = {{ABS_X, -32768, 32767, 0}, {ABS_Y, 0, 255, 255}, {ABS_Z, 0, 255, 255},
                {ABS_RZ, 0, 255, 255}};
    for (const auto& a : axes) {
      uinput_abs_setup abs{};
      abs.code = static_cast<__u16>(a.code);
      abs.absinfo.minimum = a.min;
      abs.absinfo.maximum = a.max;
      abs.absinfo.value = a.value;
      ioctl(fd, UI_SET_ABSBIT, a.code);
      ioctl(fd, UI_ABS_SETUP, &abs);
    }
    ioctl(fd, UI_SET_EVBIT, EV_FF);
    for (int f : {FF_CONSTANT, FF_GAIN, FF_AUTOCENTER}) {
      ioctl(fd, UI_SET_FFBIT, f);
    }
    uinput_setup setup{};
    setup.id = {BUS_USB, 0x046d, 0xc24f, 0x111};
    std::snprintf(setup.name, sizeof(setup.name), "Logitech G29 Driving Force Racing Wheel");
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
          ioctl(fd, UI_END_FF_UPLOAD, &up);
        } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
          uinput_ff_erase er{};
          er.request_id = static_cast<__u32>(ev.value);
          ioctl(fd, UI_BEGIN_FF_ERASE, &er);
          er.retval = 0;
          ioctl(fd, UI_END_FF_ERASE, &er);
        } else if (ev.type == EV_FF && ev.code == FF_AUTOCENTER) {
          autocenter = ev.value;
          autocenter_writes++;
          if (ev.value > 0 && ev.value < 0xFFFF * 30 / 100) {
            autocenter_between++;
          }
        }
      }
    }
  }

  // Waits for the device to hold `percent` autocentre (SDL writes 0xFFFF * percent / 100).
  bool AutocenterIs(int percent) {
    const int want = static_cast<int>(0xFFFFUL * static_cast<unsigned>(percent) / 100);
    for (int i = 0; i < 100; i++) {
      if (autocenter == want) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "autocentre is %d, expected %d\n", autocenter.load(), want);
    return false;
  }

  ~VirtualWheel() {
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

// FH1's spring (sub_82CB76B8 with ControllerFFB.ini): block 4, coefficients 63, saturations 127.
FfEffect Spring(uint32_t type = kFfSpring) {
  FfEffect e;
  e.block = 4;
  e.type = type;
  e.gain = 255;
  e.pos_coeff = e.neg_coeff = 63;
  e.pos_sat = e.neg_sat = 127;
  return e;
}

// The game polls the pad every frame; the driver steps the fade there.
void Poll(WheelInputDriver& driver, rex::input::DeviceId id, int ms) {
  for (int t = 0; t < ms; t += 10) {
    rex::input::X_INPUT_STATE state{};
    driver.GetDeviceState(id, &state);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void SetCvar(const char* name, const char* value) {
  for (rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
    if (entry.name == name) {
      entry.setter(value);
      return;
    }
  }
  Check(false, "cvar not found");
}

}  // namespace

int main() {
  setenv("SDL_JOYSTICK_IGNORE_DEVICES_EXCEPT", "0x046d/0xc24f", 1);
  VirtualWheel dev;
  Check(dev.Create(), "create the uinput wheel (needs /dev/uinput)");
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // udev
  WheelInputDriver driver(nullptr, 0);
  Check(driver.Setup() == X_STATUS_SUCCESS, "driver setup");
  std::vector<rex::input::DeviceInfo> found;
  for (int i = 0; i < 50 && found.empty(); i++) {
    driver.EnumerateDevices(found);
    if (found.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  Check(found.size() == 1, "the driver opens the virtual wheel");
  Check(dev.AutocenterIs(0), "the wheel's own autocentre is off at first");

  // Default strength 60: the spring pulls with 63/127 * 0.6, autocentre 30.
  const int spring = 30;
  Check(GameFfDeviceInfo(), "device info");
  Check(GameFfControl(kFfStopAll), "stop all before the effects");
  Check(GameFfSetEffect(Spring()), "define the spring");
  Check(dev.AutocenterIs(0), "a defined spring waits for its start");
  Check(GameFfOperate(4, 2), "start the spring");
  Check(dev.AutocenterIs(spring), "running spring: autocentre");
  Check(GameFfControl(kFfStopAll), "stop all");
  Check(dev.AutocenterIs(0), "stop all: autocentre 0");
  Check(GameFfOperate(4, 2), "restart the spring");
  Check(dev.AutocenterIs(spring), "restarted spring: autocentre");
  Check(GameFfControl(kFfReset), "reset");
  Check(dev.AutocenterIs(0), "reset: autocentre 0");
  Check(!GameFfOperate(4, 2), "a reset block cannot start");
  Check(GameFfSetEffect(Spring()) && GameFfOperate(4, 2), "define and start again");
  Check(dev.AutocenterIs(spring), "spring again: autocentre");
  Check(GameFfSetEffect(Spring(kFfDamper)), "redefine the block as a damper");
  Check(dev.AutocenterIs(0), "the block is no spring any more: autocentre 0");
  Check(GameFfSetEffect(Spring()), "back to a spring (still started)");
  Check(dev.AutocenterIs(spring), "spring once more: autocentre");
  FfEffect quiet = Spring();
  quiet.gain = 0;
  Check(GameFfUpdateEffect(quiet, 0x100), "gain 0 (a car at a standstill)");
  Check(dev.AutocenterIs(0), "gain 0: autocentre 0");

  // #256: not driving (the pause menu, a rewind, a replay): the forces fade out, then back in.
  const rex::input::DeviceId id = found[0].id;
  Check(GameFfUpdateEffect(Spring(), 0x100), "the spring at full gain again");
  Check(dev.AutocenterIs(spring), "full gain: autocentre");
  Poll(driver, id, 100);
  Check(dev.AutocenterIs(spring), "the game never said: no fade");
  ForzaWheelPlayerDriving(0);
  Poll(driver, id, 100);
  Check(dev.autocenter > 0 && dev.autocenter < 0xFFFF * spring / 100, "fading out");
  Poll(driver, id, 300);
  Check(dev.AutocenterIs(0), "not driving: the spring has faded out");
  Check(GameFfUpdateEffect(Spring(), 0x100), "the game updates its spring while not driving");
  Poll(driver, id, 50);
  Check(dev.AutocenterIs(0), "an update while not driving stays faded out");
  const int between = dev.autocenter_between;
  ForzaWheelPlayerDriving(1);
  Poll(driver, id, 700);
  Check(dev.AutocenterIs(spring), "driving again: the spring is back");
  Check(dev.autocenter_between - between >= 3, "it came back in steps");
  // wheel_ffb_spring_strength 50: half the centring (autocentre 15).
  SetCvar("wheel_ffb_spring_strength", "50");
  Check(GameFfUpdateEffect(Spring(), 0x100), "the spring again");
  Check(dev.AutocenterIs(spring / 2), "spring strength 50: half the autocentre");
  std::printf("wheel ff probe: autocentre follows the spring (%d writes), fades while not driving, "
              "spring strength; all checks passed\n",
              dev.autocenter_writes.load());
  return 0;
}
