/**
 * @file        input/tap_strum.cpp
 * @brief       A control pressed briefly when one of some others is pressed: a music game's strum on each fret
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <rex/input/tap_strum.h>

#include <algorithm>
#include <atomic>
#include <cctype>

#include <rex/input/input.h>

namespace rex::input {

namespace {

std::atomic<bool (*)()> g_tap_gate{nullptr};

}  // namespace

void SetTapGateFunction(bool (*gate)()) {
  g_tap_gate.store(gate, std::memory_order_release);
}

uint16_t TapControlBits(std::string_view list) {
  struct Name {
    std::string_view name;
    uint16_t bit;
  };
  static constexpr Name kNames[] = {
      {"a", X_INPUT_GAMEPAD_A},
      {"b", X_INPUT_GAMEPAD_B},
      {"x", X_INPUT_GAMEPAD_X},
      {"y", X_INPUT_GAMEPAD_Y},
      {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"back", X_INPUT_GAMEPAD_BACK},
      {"start", X_INPUT_GAMEPAD_START},
      {"ls", X_INPUT_GAMEPAD_LEFT_THUMB},
      {"rs", X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"dpad_up", X_INPUT_GAMEPAD_DPAD_UP},
      {"dpad_down", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"dpad_left", X_INPUT_GAMEPAD_DPAD_LEFT},
      {"dpad_right", X_INPUT_GAMEPAD_DPAD_RIGHT},
  };
  uint16_t bits = 0;
  while (!list.empty()) {
    size_t comma = list.find(',');
    std::string_view name = list.substr(0, comma);
    while (!name.empty() && name.front() == ' ')
      name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ')
      name.remove_suffix(1);
    for (const Name& n : kNames) {
      if (name.size() == n.name.size() &&
          std::equal(name.begin(), name.end(), n.name.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
          })) {
        bits |= n.bit;
      }
    }
    if (comma == std::string_view::npos)
      break;
    list.remove_prefix(comma + 1);
  }
  return bits;
}

uint16_t TapStrum::Apply(uint16_t buttons, uint16_t target, uint16_t sources, int window_ms, int hold_ms) {
  sources &= ~target;
  if (!target || !sources) {
    previous_ = pending_mask_ = active_mask_ = 0;
    pending_ = false;
    return buttons;
  }
  // The game's gate closed: no press is a tap, and the sources held are not new when it opens.
  bool (*gate)() = g_tap_gate.load(std::memory_order_acquire);
  if (gate && !gate()) {
    previous_ = buttons & sources;
    pending_mask_ = active_mask_ = 0;
    pending_ = false;
    return buttons;
  }
  using Clock = std::chrono::steady_clock;
  // The target released between two presses: a game that reads the pad once a frame (16.7 ms at 60
  // FPS) sees it.
  constexpr auto kReleased = std::chrono::milliseconds(20);
  auto now = Clock::now();
  uint16_t pressed = buttons & sources;
  uint16_t newly = pressed & ~previous_;
  previous_ = pressed;
  // A source pressed: the first opens the window in which the others join the same press (a chord
  // pressed a few milliseconds apart).
  if (newly) {
    if (!pending_) {
      pending_ = true;
      first_ = now;
      pending_mask_ = 0;
    }
    pending_mask_ |= newly;
  }
  auto window = std::chrono::milliseconds(std::max(0, window_ms));
  if (pending_ && now - first_ >= window && now >= ready_) {
    pending_ = false;
    active_mask_ = pending_mask_;
    until_ = now + std::chrono::milliseconds(std::max(1, hold_ms));
    ready_ = until_ + kReleased;
  }
  // While the target is pressed, the sources of its press stay pressed too: a key tapped and
  // released within the window still reaches the game with its strum.
  if (now < until_) {
    buttons |= target | active_mask_;
  }
  return buttons;
}

}  // namespace rex::input
