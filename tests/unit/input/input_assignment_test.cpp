#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

REXCVAR_DECLARE(int32_t, gamepad_subtype);
REXCVAR_DECLARE(bool, gamepad_whammy_rest);

// The X_STATUS_* / X_ERROR_* macros cast through these unqualified, so
// FakeDriver cannot match the base class signatures without them.
using rex::X_RESULT;
using rex::X_STATUS;

using rex::input::DeviceAssignment;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::SharedAssignment;
using rex::input::SlotAssignment;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

DeviceInfo Pad(uint64_t id, uint32_t ordinal) {
  DeviceInfo info;
  info.id = static_cast<DeviceId>(id);
  info.ordinal = ordinal;
  return info;
}

std::vector<DeviceId> For(const DeviceAssignment& a, uint32_t user) {
  std::vector<DeviceId> out;
  a.DevicesForUser(user, out);
  return out;
}

/// Exercises assignment with no SDL, window, or OS behind it.
class FakeDriver final : public InputDriver {
 public:
  FakeDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void Add(uint64_t id, bool synthetic = false) {
    DeviceInfo info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
    // As long as an SDL joystick GUID's 32 hex digits, so it lives on the heap as a real pad's does.
    info.guid = fmt::format("{:032x}", id);
    devices_.push_back(info);
    states_[static_cast<DeviceId>(id)] = {};
  }

  void Remove(uint64_t id) {
    std::erase_if(devices_, [&](const DeviceInfo& d) { return d.id == static_cast<DeviceId>(id); });
    states_.erase(static_cast<DeviceId>(id));
  }

  void SetButtons(uint64_t id, uint16_t buttons) {
    states_[static_cast<DeviceId>(id)].gamepad.buttons = buttons;
  }

  void SetRightX(uint64_t id, int16_t x) { states_[static_cast<DeviceId>(id)].gamepad.thumb_rx = x; }

  void SetWhammyBar(uint64_t id, bool bar) {
    for (DeviceInfo& d : devices_) {
      if (d.id == static_cast<DeviceId>(id)) d.whammy_bar = bar;
    }
  }

  void QueueKeystroke(uint64_t id, uint16_t vk) {
    X_INPUT_KEYSTROKE ks = {};
    ks.virtual_key = vk;
    keystrokes_[static_cast<DeviceId>(id)] = ks;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override { out = devices_; }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    auto it = states_.find(id);
    if (it == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_state = it->second;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    // Stamp the id so the test can tell which device answered.
    out->sub_type = static_cast<uint8_t>(id);
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return states_.count(id) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    auto it = keystrokes_.find(id);
    if (it == keystrokes_.end()) {
      return X_ERROR_EMPTY;
    }
    *out = it->second;
    return X_ERROR_SUCCESS;
  }

 private:
  std::vector<DeviceInfo> devices_;
  std::map<DeviceId, X_INPUT_STATE> states_;
  std::map<DeviceId, X_INPUT_KEYSTROKE> keystrokes_;
};

std::unique_ptr<InputSystem> SharedSystem(FakeDriver** out_driver) {
  auto driver = std::make_unique<FakeDriver>();
  *out_driver = driver.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(driver));
  system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return system;
}

}  // namespace

TEST_CASE("SharedAssignment puts every device on user 0", "[input]") {
  SharedAssignment shared;
  shared.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});

  REQUIRE(For(shared, 0) ==
          std::vector<DeviceId>{static_cast<DeviceId>(10), static_cast<DeviceId>(11)});
  REQUIRE(For(shared, 1).empty());
}

TEST_CASE("SlotAssignment does not renumber survivors when a pad is unplugged", "[input]") {
  SlotAssignment slots;
  slots.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});
  slots.OnDevicesChanged({Pad(11, 1)});

  REQUIRE(For(slots, 0).empty());
  REQUIRE(For(slots, 1) == std::vector<DeviceId>{static_cast<DeviceId>(11)});
}

TEST_CASE("Both controllers reach one guest user", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
}

TEST_CASE("Unplugging one controller leaves the other working", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Remove(1);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("Capabilities follow the controller in hand", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  REQUIRE(caps.sub_type == 2);
}

TEST_CASE("A keystroke from the second controller reaches the guest", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->QueueKeystroke(2, 0x5800);

  X_INPUT_KEYSTROKE ks = {};
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(ks.virtual_key) == 0x5800);
}

TEST_CASE("A reconnected pad comes back to guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  driver->Add(1);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  // Replugging arrives as a new device id, not as the old one coming back.
  driver->Remove(1);
  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("A synthetic device does not push the first pad off guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  // The NOP stand-in is always present and enumerates before any pad connects.
  driver->Add(1, true);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("Several threads read the controllers at once", "[input]") {
  // A title reads the pads on one thread while another asks who is signed in (a guest's check enumerates them too)
  // or sets a vibration, and the quit prompt reads them on the UI thread: every read enumerates the devices again.
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);

  std::atomic<bool> failed{false};
  auto reader = [&](int kind) {
    for (int i = 0; i < 20000; i++) {
      X_INPUT_STATE state = {};
      X_INPUT_CAPABILITIES caps = {};
      X_INPUT_VIBRATION vibration = {};
      bool ok = true;
      switch (kind) {
        case 0:
          ok = system->GetState(0, &state) == X_ERROR_SUCCESS &&
               static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_A;
          break;
        case 1:
          ok = system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS;
          break;
        case 2:
          ok = system->UserHasDevice(0);
          break;
        case 3:
          ok = system->SetState(0, &vibration) == X_ERROR_SUCCESS;
          break;
        default:
          ok = system->GetStateForUI(0, &state) == X_ERROR_SUCCESS;
          break;
      }
      if (!ok) {
        failed = true;
      }
    }
  };
  std::vector<std::thread> threads;
  for (int kind = 0; kind < 5; kind++) {
    threads.emplace_back(reader, kind);
  }
  for (auto& thread : threads) {
    thread.join();
  }
  REQUIRE_FALSE(failed);
}

TEST_CASE("A gamepad played as a guitar keeps a whammy bar's rest", "[input]") {
  const int32_t subtype = REXCVAR_GET(gamepad_subtype);
  const bool whammy_rest = REXCVAR_GET(gamepad_whammy_rest);
  REXCVAR_SET(gamepad_subtype, 6);
  REXCVAR_SET(gamepad_whammy_rest, true);
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  X_INPUT_STATE state = {};

  // A stick: in the middle the bar is at rest, at either side all the way.
  driver->SetRightX(1, 0);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<int16_t>(state.gamepad.thumb_rx) == -32768);
  driver->SetRightX(1, -32768);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<int16_t>(state.gamepad.thumb_rx) == 32767);

  // A bar already (a guitar that reports itself as a gamepad, found resting at -32768, or calibrated): its own value.
  driver->SetWhammyBar(1, true);
  driver->SetRightX(1, -32768);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<int16_t>(state.gamepad.thumb_rx) == -32768);
  driver->SetRightX(1, 32767);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<int16_t>(state.gamepad.thumb_rx) == 32767);

  REXCVAR_SET(gamepad_subtype, subtype);
  REXCVAR_SET(gamepad_whammy_rest, whammy_rest);
}
