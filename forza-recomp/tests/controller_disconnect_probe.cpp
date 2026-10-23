// Exercise the patched SDK's real enumeration/assignment/active-device path.
// A fixture driver makes unplugging reproducible without physical hardware.
#include <rex/input/input_system.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <unordered_map>

extern "C" uint32_t ForzaInputDisconnectSequence(uint32_t user_mask);
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
constexpr DeviceId keyboard = DeviceId(101), pad = DeviceId(102), spare = DeviceId(103);
struct Driver final : InputDriver {
  Driver() : InputDriver(nullptr, 0) {}
  std::vector<DeviceInfo> devices;
  std::unordered_map<DeviceId, uint16_t> buttons;
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override { out = devices; }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* state) override {
    *state = {}; state->gamepad.buttons = buttons[id]; return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES*) override {
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_SUCCESS; }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }
};
DeviceInfo Device(DeviceId id, bool synthetic = false) {
  DeviceInfo info; info.id = id; info.synthetic = synthetic; return info;
}
int main() {
  InputSystem input(nullptr);
  auto driver = std::make_unique<Driver>(); auto* raw = driver.get();
  input.AddDriver(std::move(driver));
  input.SetDeviceAssignment(std::make_unique<SharedAssignment>());
  X_INPUT_STATE state{};
  auto poll = [&] { assert(input.GetStateForUI(0, &state) == X_ERROR_SUCCESS); };
  const auto initial = ForzaInputDisconnectSequence(1);
  raw->devices = {Device(keyboard, true), Device(pad), Device(spare)};
  raw->buttons[pad] = X_INPUT_GAMEPAD_A;
  poll();
  raw->devices.pop_back();  // unused spare
  poll(); assert(ForzaInputDisconnectSequence(1) == initial);
  raw->devices.pop_back();  // active physical pad, keyboard still connected
  poll(); assert(ForzaInputDisconnectSequence(1) == initial + 1);
  assert(input.GetConnectedUsers()[0]);
  assert(ForzaInputDisconnectSequence(14) == 0);  // other guest users
  poll(); assert(ForzaInputDisconnectSequence(1) == initial + 1);
  raw->devices.push_back(Device(spare));  // reconnect alone is not a loss
  poll(); assert(ForzaInputDisconnectSequence(1) == initial + 1);
  raw->buttons[spare] = X_INPUT_GAMEPAD_A; poll();
  raw->buttons[spare] = 0; raw->buttons[keyboard] = X_INPUT_GAMEPAD_B; poll();
  raw->devices.pop_back();  // keyboard now in use: unplugging the idle pad is harmless
  poll(); assert(ForzaInputDisconnectSequence(1) == initial + 1);
  raw->devices = {Device(pad)}; raw->buttons[pad] = X_INPUT_GAMEPAD_A;
  poll(); assert(ForzaInputDisconnectSequence(1) == initial + 1);  // synthetic removal
  raw->devices.clear();
  assert(input.GetStateForUI(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  assert(ForzaInputDisconnectSequence(1) == initial + 2);
  std::fprintf(stderr, "SDK disconnect probe: active loss, synthetic input, idle spare, reconnect and masks pass\n");
}
