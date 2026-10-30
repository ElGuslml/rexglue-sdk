/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

#include <rex/input/input_driver.h>

#include <SDL3/SDL.h>

#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

  // The Xbox 360 controls gamepad_remap can move: A B X Y LB RB LT RT LS RS Start Back Guide (the D-pad stays).
  static constexpr int kRemapControls = 13;

  // A whammy bar a launcher calibrated (gamepad_whammy_calibration): the SDL_GamepadAxis it is on, and that axis's
  // SDL value at rest and pressed all the way.
  struct WhammyCalibration {
    int axis = -1;  // -1: none
    int32_t rest = 0;
    int32_t full = 0;
  };
  // Where a gamepad's right stick's X axis rests, judged once the axis has stayed put for a second.
  enum class RightXRest : uint8_t { kUnknown, kStick, kBar };

 private:
  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    DeviceId id;
    // Per pad rather than per guest user, so it survives reassignment.
    KeystrokeState keystroke;
    // What the controller itself reports: the SDL_GamepadButton bits held and the two triggers (0-255). The
    // guest's buttons and triggers are computed from them through `sources` (gamepad_remap).
    uint64_t physical_buttons;
    uint8_t physical_triggers[2];
    // For each remappable control, the physical inputs that press it (SDL_GamepadButton bits; bits 32 and 33
    // are the left and right triggers).
    std::array<uint64_t, kRemapControls> sources;
    // A guitar whose whammy bar (the right stick's X axis, -32768 at rest) has not reported a position yet: an Xbox
    // 360 guitar reads 0 there, half pressed, until the bar first moves. Until then the bar is at rest (with
    // guitar_whammy_rest).
    bool whammy_unknown;
    // Its calibrated whammy bar: the right stick's X is that axis, from rest (-32768) to all the way (32767).
    WhammyCalibration whammy;
    // Where its right stick's X rests (a gamepad played as a guitar, gamepad_whammy_rest: a guitar that reports
    // itself as a gamepad rests its bar at -32768, a stick in the middle), and since when the axis has stayed near
    // `right_x_still_value` (empty: it has reported nothing yet).
    RightXRest right_x_rest;
    int32_t right_x_still_value;
    std::chrono::steady_clock::time_point right_x_still_since;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnJoystickDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* FindController(DeviceId id);
  DeviceId AllocateDeviceId();
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  // The guest's buttons and triggers from the physical ones, through gamepad_remap.
  static void ApplyRemap(ControllerState& state);
  void QueueControllerUpdate();

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  // Appended in connection order, never re-sorted, and unbounded: the
  // assignment decides how many the guest sees.
  std::vector<ControllerState> controllers_;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  // Never rewound, so a handle held past a disconnect resolves to nothing.
  uint64_t next_device_id_ = 1;
};

}  // namespace rex::input::sdl
