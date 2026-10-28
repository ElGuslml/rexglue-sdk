/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>

#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");

REXCVAR_DEFINE_BOOL(vibration, true, "Input", "Enable controller vibration");

REXCVAR_DEFINE_DOUBLE(left_stick_deadzone_percentage, 0.0, "Input",
                      "Deadzone applied to the left stick, as a fraction of its range")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(right_stick_deadzone_percentage, 0.0, "Input",
                      "Deadzone applied to the right stick, as a fraction of its range")
    .range(0.0, 1.0);
REXCVAR_DEFINE_INT32(gamepad_subtype, 0, "Input",
                     "XINPUT_DEVSUBTYPE reported for a plain gamepad (and the keyboard's emulated pad): "
                     "0 = gamepad, 6 = guitar, 7 = alternate guitar, 8 = drum kit, 11 = bass guitar. "
                     "Devices of another kind (a real guitar) keep their own")
    .range(0, 0x13);
REXCVAR_DEFINE_INT32(guitar_subtype, 0, "Input",
                     "XINPUT_DEVSUBTYPE reported for a guitar: 0 = the driver's (SDL reports every guitar as 6, "
                     "guitar), 7 = alternate guitar (Guitar Hero's guitars, which Guitar Hero III's engine requires)")
    .range(0, 0x13);
REXCVAR_DEFINE_STRING(gamepad_device_subtypes, "", "Input",
                      "XINPUT_DEVSUBTYPE reported for particular gamepads, in place of gamepad_subtype: "
                      "controller=subtype entries separated by commas, a controller named by its serial "
                      "number or else its SDL GUID (as the log's OnControllerDeviceAdded lines give them; "
                      "keyboard = the keyboard's emulated pad), 1 = gamepad. A launcher writes it");
namespace rex::input {

namespace {

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

// XN_SYS_INPUTDEVICESCHANGED
constexpr uint32_t kXNotificationSystemInputDevicesChanged = 0x00000012;

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '	')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '	')) {
    text.remove_suffix(1);
  }
  return text;
}

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

// The subtype gamepad_device_subtypes gives a device, -1 when it does not name it. A serial number
// tells two pads of one model apart, which share a GUID; a pad without one (XInput's) goes by its GUID.
int32_t DeviceSubtypeSetting(const DeviceInfo& device) {
  const std::string& text = REXCVAR_GET(gamepad_device_subtypes);
  int32_t by_guid = -1;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string_view entry(text.data() + start, end - start);
    start = end + 1;
    const size_t equals = entry.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    const std::string_view key = Trim(entry.substr(0, equals));
    const std::string_view value = Trim(entry.substr(equals + 1));
    int32_t subtype = -1;
    if (std::from_chars(value.data(), value.data() + value.size(), subtype).ec != std::errc() ||
        subtype < 0 || subtype > 0x13) {
      continue;
    }
    if (device.synthetic) {
      if (EqualsIgnoringCase(key, "keyboard")) {
        return subtype;
      }
      continue;
    }
    if (!device.serial.empty() && EqualsIgnoringCase(key, device.serial)) {
      return subtype;
    }
    if (!device.guid.empty() && EqualsIgnoringCase(key, device.guid)) {
      by_guid = subtype;
    }
  }
  return by_guid;
}

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

DeviceId InputSystem::ChooseDeviceForUser(uint32_t user_index) const {
  if (!assignment_) {
    return DeviceId::kInvalid;
  }
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return DeviceId::kInvalid;
  }
  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }
  return chosen;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  DeviceId chosen = ChooseDeviceForUser(user_index);
  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT result = driver->GetDeviceCapabilities(chosen, flags, out_caps);
  if (result != X_ERROR_SUCCESS) {
    return result;
  }
  // A music game asks for the kind of controller and gives a plain gamepad another part (Guitar Hero's
  // band games make it the singer, who needs a microphone). Reported as a guitar or a drum kit, the pad
  // plays that part with the same XInput controls those instruments use (frets on A, B, Y, X and LB,
  // strumming on the D-pad, the whammy bar on the right stick).
  const int32_t subtype = REXCVAR_GET(gamepad_subtype);
  // SDL gives every guitar one joystick type, so the SDL driver reports them all as XINPUT_DEVSUBTYPE_GUITAR,
  // Rock Band's subtype; Guitar Hero's guitars are XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE on the console.
  // Guitar Hero III's engine takes only the latter as a guitar and calls the former unsupported.
  const int32_t guitar = REXCVAR_GET(guitar_subtype);
  // A pad of its own part (Guitar Hero's singer stays a gamepad while the others play guitars).
  const DeviceInfo* info = DeviceInfoFor(chosen);
  const int32_t own = info ? DeviceSubtypeSetting(*info) : -1;
  if (own >= 0 && out_caps->sub_type == XINPUT_DEVSUBTYPE_GAMEPAD) {
    if (own > XINPUT_DEVSUBTYPE_GAMEPAD) {
      out_caps->sub_type = static_cast<uint8_t>(own);
    }
  } else if (subtype != 0 && out_caps->sub_type == XINPUT_DEVSUBTYPE_GAMEPAD) {
    out_caps->sub_type = static_cast<uint8_t>(subtype);
  } else if (guitar != 0 && out_caps->sub_type == XINPUT_DEVSUBTYPE_GUITAR) {
    out_caps->sub_type = static_cast<uint8_t>(guitar);
  }
  return result;
}

void InputSystem::UpdateConnectedUser(uint32_t user_index, bool connected) {
  if (user_index >= kMaxGuestUsers || connected_users_.test(user_index) == connected) {
    return;
  }
  connected_users_.set(user_index, connected);
  if (connected) {
    REXLOG_INFO("New controller connected to slot {}.", user_index);
  } else {
    REXLOG_INFO("Controller disconnected from slot {}.", user_index);
  }

  // Titles poll capabilities off this rather than every frame, so without it
  // a pad plugged in mid-game is never noticed.
  if (auto* kernel_state = REX_KERNEL_STATE()) {
    kernel_state->BroadcastNotification(kXNotificationSystemInputDevicesChanged, 0);
  }

  if (!connected) {
    user_max_joystick_value_[user_index] = {};
    consumed_buttons_[user_index] = 0;
    return;
  }

  // Deadzone percentages scale against the device's own range.
  DeviceId chosen = ChooseDeviceForUser(user_index);
  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return;
  }
  X_INPUT_CAPABILITIES caps = {};
  if (driver->GetDeviceCapabilities(chosen, 0, &caps) != X_ERROR_SUCCESS) {
    return;
  }
  user_max_joystick_value_[user_index] = {{caps.gamepad.thumb_lx, caps.gamepad.thumb_ly},
                                          {caps.gamepad.thumb_rx, caps.gamepad.thumb_ry}};
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  // A dialog owns the controller.
  if (ui_input_blockers_.load() > 0) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT result = GetStateForUI(user_index, out_state);

  // SetPrimaryPressWindow: a new press on another user's controller makes it user 0's. This read sees that user's
  // new controller (user 0's old one) at rest; user 0's next read sees the press.
  if (result == X_ERROR_SUCCESS && out_state && user_index < kMaxGuestUsers && primary_press_window_.load()) {
    const uint16_t buttons = static_cast<uint16_t>(out_state->gamepad.buttons);
    const uint16_t pressed = static_cast<uint16_t>(buttons & ~window_buttons_[user_index]);
    window_buttons_[user_index] = buttons;
    if (user_index != 0 && pressed != 0 && MakePrimary(user_index)) {
      window_buttons_[0] = 0;
      window_buttons_[user_index] = 0xFFFF;
      std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
      return result;
    }
  }

  if (result == X_ERROR_SUCCESS && out_state && user_index < kMaxGuestUsers &&
      consumed_buttons_[user_index] != 0) {
    const uint16_t buttons = out_state->gamepad.buttons;
    // Each button leaves the mask once the player lets go of it.
    consumed_buttons_[user_index] &= buttons;
    out_state->gamepad.buttons = static_cast<uint16_t>(buttons & ~consumed_buttons_[user_index]);
  }

  // A host UI watching the guest's reads (SetHostInputFilter) can take this one: the guest reads the pad at rest,
  // and the buttons held stay masked until released.
  if (result == X_ERROR_SUCCESS && out_state) {
    bool take = false;
    {
      std::lock_guard lock(host_filter_mutex_);
      if (host_filter_) {
        take = host_filter_(user_index, out_state->gamepad);
      }
    }
    if (take) {
      if (user_index < kMaxGuestUsers) {
        consumed_buttons_[user_index] |= static_cast<uint16_t>(out_state->gamepad.buttons);
      }
      std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
    }
  }

  return result;
}

X_RESULT InputSystem::GetStateForUI(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    UpdateConnectedUser(user_index, false);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  UpdateConnectedUser(user_index, true);
  AdjustDeadzoneLevels(user_index, &merged.gamepad);
  if (static_cast<uint16_t>(merged.gamepad.buttons) != 0 && user_index < kMaxGuestUsers) {
    last_used_user_ = user_index;
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

void InputSystem::AddUIInputBlocker() {
  ui_input_blockers_.fetch_add(1);
}

void InputSystem::RemoveUIInputBlocker() {
  // Whatever is held right now stays masked until released, so the press that
  // dismissed the dialog is not also read as a press in the game.
  X_INPUT_STATE state = {};
  for (uint32_t user_index = 0; user_index < kMaxGuestUsers; user_index++) {
    if (GetStateForUI(user_index, &state) == X_ERROR_SUCCESS) {
      consumed_buttons_[user_index] |= static_cast<uint16_t>(state.gamepad.buttons);
    }
  }

  ui_input_blockers_.fetch_sub(1);
}

void InputSystem::SetHostInputFilter(HostInputFilter filter) {
  std::lock_guard lock(host_filter_mutex_);
  host_filter_ = std::move(filter);
}

void InputSystem::SetPrimaryPressWindow(bool open) {
  if (primary_press_window_.exchange(open) == open || !open) {
    return;
  }
  // Opening: whatever is held now is not a new press until it is released.
  window_buttons_.fill(0xFFFF);
}

bool InputSystem::UserHasDevice(uint32_t user_index) {
  if (!assignment_ || user_index >= kMaxGuestUsers) {
    return false;
  }
  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  return !ids.empty();
}

bool InputSystem::MakePrimary(uint32_t user_index) {
  if (!assignment_ || user_index == 0 || user_index >= kMaxGuestUsers) {
    return false;
  }
  // The user's physical controller (the keyboard's pad is always user 0's) and its ordinal, which SlotAssignment
  // makes its user index.
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  uint32_t ordinal = UINT32_MAX;
  for (const auto& device : devices_) {
    if (!device.synthetic && std::find(ids.begin(), ids.end(), device.id) != ids.end()) {
      ordinal = device.ordinal;
      break;
    }
  }
  if (ordinal == UINT32_MAX || ordinal == 0) {
    return false;
  }
  for (auto& device : devices_) {
    if (device.synthetic) {
      continue;
    }
    if (device.ordinal == ordinal) {
      device.ordinal = 0;
    } else if (device.ordinal == 0) {
      device.ordinal = ordinal;
    }
  }
  std::vector<size_t> order(devices_.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return devices_[a].ordinal < devices_[b].ordinal; });
  std::vector<DeviceInfo> devices;
  std::vector<InputDriver*> owners;
  for (size_t i : order) {
    devices.push_back(devices_[i]);
    owners.push_back(device_owners_[i]);
  }
  devices_ = std::move(devices);
  device_owners_ = std::move(owners);
  assignment_->OnDevicesChanged(devices_);

  std::swap(user_max_joystick_value_[0], user_max_joystick_value_[user_index]);
  consumed_buttons_[0] = 0;
  consumed_buttons_[user_index] = 0;
  const bool was_connected = connected_users_.test(0);
  connected_users_.set(0, true);
  connected_users_.set(user_index, was_connected);
  REXLOG_INFO("The controller in slot {} pressed first: it is now slot 0, the profile's (slot 0's controller moves to "
              "slot {}).",
              user_index, user_index);

  // The guest asks for the controllers' kinds again (a guitar and a pad swapped).
  if (auto* kernel_state = REX_KERNEL_STATE()) {
    kernel_state->BroadcastNotification(kXNotificationSystemInputDevicesChanged, 0);
  }
  return true;
}

bool InputSystem::GetVibrationEnabled() const {
  return REXCVAR_GET(vibration);
}

void InputSystem::ToggleVibration() {
  REXCVAR_SET(vibration, !REXCVAR_GET(vibration));
  // The guest's next SetState may never come while a motor is running.
  X_INPUT_VIBRATION silence = {};
  for (uint32_t user_index = 0; user_index < kMaxGuestUsers; user_index++) {
    SetState(user_index, &silence);
  }
}

void InputSystem::AdjustDeadzoneLevels(uint32_t user_index, X_INPUT_GAMEPAD* gamepad) const {
  if (user_index >= kMaxGuestUsers || !gamepad) {
    return;
  }

  // Radial: the cutoff follows the stick's angle, so a diagonal is not held to
  // a larger displacement than a cardinal push.
  auto apply = [](double percentage, const JoystickValue& max, int16_t& axis_x, int16_t& axis_y) {
    if (percentage <= 0.0 || percentage >= 1.0) {
      return;
    }
    const double deadzone_x = max.first * percentage;
    const double deadzone_y = max.second * percentage;
    const double theta = std::atan2(static_cast<double>(axis_y), static_cast<double>(axis_x));
    const double cutoff_x = std::cos(theta) * deadzone_x;
    const double cutoff_y = std::sin(theta) * deadzone_y;
    if (axis_x > -cutoff_x && axis_x < cutoff_x) {
      axis_x = 0;
    }
    if (axis_y > -cutoff_y && axis_y < cutoff_y) {
      axis_y = 0;
    }
  };

  const auto& maxima = user_max_joystick_value_[user_index];
  int16_t lx = gamepad->thumb_lx;
  int16_t ly = gamepad->thumb_ly;
  apply(REXCVAR_GET(left_stick_deadzone_percentage), maxima.first, lx, ly);
  gamepad->thumb_lx = lx;
  gamepad->thumb_ly = ly;

  int16_t rx = gamepad->thumb_rx;
  int16_t ry = gamepad->thumb_ry;
  apply(REXCVAR_GET(right_stick_deadzone_percentage), maxima.second, rx, ry);
  gamepad->thumb_rx = rx;
  gamepad->thumb_ry = ry;
}

X_INPUT_VIBRATION InputSystem::ModifyVibrationLevel(const X_INPUT_VIBRATION* vibration) const {
  X_INPUT_VIBRATION modified = *vibration;
  if (!REXCVAR_GET(vibration)) {
    modified.left_motor_speed = 0;
    modified.right_motor_speed = 0;
  }
  return modified;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_ || !vibration) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  const X_INPUT_VIBRATION modified = ModifyVibrationLevel(vibration);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_INPUT_VIBRATION per_device = modified;
    X_RESULT result = driver->SetDeviceVibration(id, &per_device);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  // A dialog owns the controller (AddUIInputBlocker): no keystrokes for the guest either, as GetState gives it a
  // pad at rest.
  if (ui_input_blockers_.load() > 0) {
    return X_ERROR_EMPTY;
  }
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

void SetPrimaryPressWindow(bool open) {
  auto* kernel_state = REX_KERNEL_STATE();
  if (!kernel_state || !kernel_state->emulator()) {
    return;
  }
  if (auto* input = static_cast<InputSystem*>(kernel_state->emulator()->input_system())) {
    input->SetPrimaryPressWindow(open);
  }
}

}  // namespace rex::input
