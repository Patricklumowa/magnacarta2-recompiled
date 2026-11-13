// Reproduce simultaneous guest/UI polling during controller hotplug, without
// requiring a physical controller or a game window.
#include <atomic>
#include <cstdio>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include <rex/input/input_system.h>

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;

class TestController final : public InputDriver {
 public:
  TestController() : InputDriver(nullptr, 0) {}
  std::atomic<bool> connected{true};
  std::atomic<unsigned> vibration_calls{0};

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& devices) override {
    DeviceInfo fallback;
    fallback.id = static_cast<DeviceId>(1);
    fallback.synthetic = true;
    fallback.name = "Synthetic neutral controller used during physical disconnects";
    devices.push_back(fallback);
    if (connected.load()) {
      DeviceInfo physical;
      physical.id = static_cast<DeviceId>(2);
      physical.name = "Physical controller repeatedly connected during concurrent polling";
      devices.push_back(physical);
    }
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* state) override {
    *state = {};
    if (id == static_cast<DeviceId>(2)) {
      state->gamepad.buttons = X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_DPAD_DOWN;
      state->gamepad.thumb_lx = 16000;
      state->gamepad.left_trigger = 200;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    *caps = {};
    caps->gamepad.thumb_lx = caps->gamepad.thumb_ly = 32767;
    caps->gamepad.thumb_rx = caps->gamepad.thumb_ry = 32767;
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    if (id == static_cast<DeviceId>(2)) ++vibration_calls;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }
};

int main() {
  InputSystem input(nullptr);
  auto controller = std::make_unique<TestController>();
  auto* device = controller.get();
  input.AddDriver(std::move(controller));
  input.SetDeviceAssignment(std::make_unique<SlotAssignment>());

  auto verify_state = [&] {
    X_INPUT_STATE state{};
    return input.GetState(0, &state) == X_ERROR_SUCCESS &&
           static_cast<uint16_t>(state.gamepad.buttons) ==
               (X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_DPAD_DOWN) &&
           static_cast<int16_t>(state.gamepad.thumb_lx) == 16000 &&
           state.gamepad.left_trigger == 200;
  };
  if (!verify_state()) return 1;
  X_INPUT_VIBRATION vibration{};
  vibration.left_motor_speed = 1000;
  if (input.SetState(0, &vibration) != X_ERROR_SUCCESS || !device->vibration_calls.load()) return 2;
  device->connected = false;
  X_INPUT_STATE neutral{};
  if (input.GetState(0, &neutral) != X_ERROR_SUCCESS || neutral.gamepad.buttons != 0) return 3;

  constexpr unsigned iterations = 20000;
  std::latch start(4);
  std::vector<std::thread> threads;
  for (unsigned worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&, worker] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < iterations; ++i) {
        X_INPUT_STATE state{};
        X_INPUT_CAPABILITIES caps{};
        X_INPUT_KEYSTROKE stroke{};
        X_INPUT_VIBRATION motors{};
        if (worker == 0) input.GetState(0, &state);
        if (worker == 1) {
          input.GetCapabilities(0, 0, &caps);
          input.SetState(0, &motors);
        }
        if (worker == 2) {
          input.GetStateForUI(0, &state);
          input.GetKeystroke(0, 0, &stroke);
        }
        if (worker == 3) {
          device->connected = (i % 2) == 0;
          input.GetState(0, &state);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  device->connected = true;
  if (!verify_state()) return 4;
  input.Shutdown();
  std::printf("Controller check passed: buttons, stick, trigger, vibration, disconnect/reconnect, "
              "and %u concurrent input operations.\n", iterations * 6);
}
