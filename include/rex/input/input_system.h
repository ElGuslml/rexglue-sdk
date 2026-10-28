#pragma once
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

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/input/tap_strum.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  /// GetState for the emulator's own UI, which reads while the guest is blocked.
  X_RESULT GetStateForUI(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  /// While any blocker is held the guest reads a neutral pad. Buttons still
  /// held when the last one drops stay masked until released, so the press
  /// that dismissed the dialog does not also reach the game.
  void AddUIInputBlocker();
  void RemoveUIInputBlocker();

  /// A host UI that watches the guest's reads, such as the quit prompt (rex/ui/overlay/quit_prompt.h): `filter`
  /// sees each user's pad whenever the guest reads it (GetState, not while a blocker is held) and returns true to
  /// take that read: the guest gets the pad at rest, and the buttons held stay masked until released, as after a
  /// dialog. A UI that then owns the controller holds a blocker (AddUIInputBlocker) and reads with GetStateForUI.
  /// One filter at a time; an empty function removes it. Called on the guest's threads.
  using HostInputFilter = std::function<bool(uint32_t user_index, const X_INPUT_GAMEPAD& gamepad)>;
  void SetHostInputFilter(HostInputFilter filter);

  bool GetVibrationEnabled() const;
  void ToggleVibration();

  /// The player who starts the game gets the profile, as on the console, where the controller that presses Start
  /// signs in: while the window is open (a game opens it on its title screen), a new button press on a controller
  /// that is not user 0's makes that controller user 0, the signed-in profile's, and moves user 0's controller to
  /// the user it leaves. The guest does not see that press on the old user; it sees it on user 0 at its next read.
  /// Closed by default. Called on the guest's threads.
  void SetPrimaryPressWindow(bool open);
  /// Whether a controller (or the keyboard's pad) is assigned to the user.
  bool UserHasDevice(uint32_t user_index);

  std::bitset<kMaxGuestUsers> GetConnectedUsers() const { return connected_users_; }
  /// Guest user whose device most recently produced a button press.
  uint32_t GetLastUsedUser() const { return last_used_user_; }

 private:
  using JoystickValue = std::pair<uint16_t, uint16_t>;
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;
  /// The device that speaks for a user, preferring the one most recently in
  /// the player's hands.
  DeviceId ChooseDeviceForUser(uint32_t user_index) const;

  void UpdateConnectedUser(uint32_t user_index, bool connected);
  /// SetPrimaryPressWindow's swap: the user's controller becomes user 0's and user 0's takes its user.
  bool MakePrimary(uint32_t user_index);
  void AdjustDeadzoneLevels(uint32_t user_index, X_INPUT_GAMEPAD* gamepad) const;
  /// A gamepad, or the keyboard's pad, that the game is told is a guitar, read as one: its whammy bar at rest while
  /// the right stick is (gamepad_whammy_rest), and its frets strumming as they are pressed (gamepad_tap_target).
  /// True when its right stick's X is then a whammy bar at rest at -32768 (a real guitar's too), for the merge.
  bool PlayAsGuitar(const DeviceInfo& device, X_INPUT_GAMEPAD* gamepad);
  X_INPUT_VIBRATION ModifyVibrationLevel(const X_INPUT_VIBRATION* vibration) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  std::bitset<kMaxGuestUsers> connected_users_;
  uint32_t last_used_user_ = 0;
  // {left, right} stick maxima, scaling the deadzone percentages.
  std::array<std::pair<JoystickValue, JoystickValue>, kMaxGuestUsers> user_max_joystick_value_ = {};

  std::atomic<int> ui_input_blockers_{0};
  // Masked out per user until the guest sees them released.
  std::array<uint16_t, kMaxGuestUsers> consumed_buttons_ = {};

  // gamepad_tap_target's, per gamepad.
  std::mutex pad_taps_mutex_;
  std::map<DeviceId, TapStrum> pad_taps_;

  std::mutex host_filter_mutex_;
  HostInputFilter host_filter_;

  std::atomic<bool> primary_press_window_{false};
  // The buttons each user's last read had while the window is open, to tell a new press from a held button.
  std::array<uint16_t, kMaxGuestUsers> window_buttons_ = {};
};

/// Create a default InputSystem with SDL + MnK + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

/// The runtime's InputSystem::SetPrimaryPressWindow, for a game's hooks; nothing without an input system.
void SetPrimaryPressWindow(bool open);

}  // namespace rex::input
