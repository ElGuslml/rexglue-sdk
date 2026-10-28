#pragma once
/**
 * @file        rex/input/tap_strum.h
 * @brief       A control pressed briefly when one of some others is pressed: a music game's strum on each fret
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <chrono>
#include <cstdint>
#include <string_view>

namespace rex::input {

/// XINPUT_GAMEPAD button bits of a comma-separated list of the pad's controls (a, b, x, y, lb, rb, back, start, ls,
/// rs, dpad_up, dpad_down, dpad_left, dpad_right).
uint16_t TapControlBits(std::string_view list);

/// The game's gate of every device's taps (the keyboard's pad's and the gamepads'): taps only while it returns true;
/// null, the default: always. It runs on the thread that reads the pad, at each read.
void SetTapGateFunction(bool (*gate)());

/// One device's taps: `target` pressed for `hold_ms` when one of `sources` is pressed, the sources pressed within
/// `window_ms` of the first making one press (a chord, one strum), which comes when the window ends; the target is
/// then released at least 20 ms, so that a game reading the pad once a frame sees each press. While the target is
/// pressed, the sources of its press stay pressed too: a button tapped and released within the window still reaches
/// the game with its strum.
class TapStrum {
 public:
  /// The device's buttons with its taps; once per read of the device, the caller serializing the calls.
  uint16_t Apply(uint16_t buttons, uint16_t target, uint16_t sources, int window_ms, int hold_ms);

 private:
  // The sources pressed at the last read, the ones pressed for the coming press of the target and for the current
  // one, and when the coming press began, when the current one ends and when the next may start.
  uint16_t previous_ = 0;
  uint16_t pending_mask_ = 0;
  uint16_t active_mask_ = 0;
  bool pending_ = false;
  std::chrono::steady_clock::time_point first_{};
  std::chrono::steady_clock::time_point until_{};
  std::chrono::steady_clock::time_point ready_{};
};

}  // namespace rex::input
