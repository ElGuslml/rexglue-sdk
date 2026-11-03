#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(std::string, keybind_a);
REXCVAR_DECLARE(std::string, keybind_b);
REXCVAR_DECLARE(std::string, keybind_back);
REXCVAR_DECLARE(std::string, keybind_dpad_left);

// The X_ERROR_* macros cast through it unqualified.
using rex::X_RESULT;

using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_BACK;
using rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
using rex::input::X_INPUT_STATE;
using rex::input::mnk::MnkInputDriver;
using rex::ui::KeyEvent;
using rex::ui::VirtualKey;

namespace {

// The driver's own handle (mnk_input_driver.cpp).
constexpr auto kKeyboard = static_cast<rex::input::DeviceId>(0x4D4E4B00);

/// The keyboard's controller with no window: keys come as the SDL window sends them (a modifier with its side).
class Keyboard {
 public:
  Keyboard() {
    REXCVAR_SET(mnk_mode, true);
    REXCVAR_SET(keybind_a, "D");
    REXCVAR_SET(keybind_b, "F");
    REXCVAR_SET(keybind_back, "Space");
    REXCVAR_SET(keybind_dpad_left, "Shift+Left");
  }
  ~Keyboard() {
    REXCVAR_SET(mnk_mode, false);
    REXCVAR_SET(keybind_a, "Semicolon,Space");
    REXCVAR_SET(keybind_b, "Quote,Backspace");
    REXCVAR_SET(keybind_back, "Z,Tab");
    REXCVAR_SET(keybind_dpad_left, "Shift+Left");
  }

  void Key(VirtualKey vk, bool down, VirtualKey side = VirtualKey::kNone) {
    KeyEvent e(nullptr, vk, 1, false, false, false, false, false);
    e.set_side_key(side);
    if (down) {
      driver_.OnKeyDown(e);
    } else {
      driver_.OnKeyUp(e);
    }
  }

  uint16_t Buttons() {
    X_INPUT_STATE state = {};
    REQUIRE(driver_.GetDeviceState(kKeyboard, &state) == X_ERROR_SUCCESS);
    return state.gamepad.buttons;
  }

 private:
  MnkInputDriver driver_{nullptr, 0};
};

}  // namespace

TEST_CASE("A modifier bound alone does not silence the other keys", "[input][mnk]") {
  Keyboard keyboard;
  REXCVAR_SET(keybind_back, "LAlt");

  keyboard.Key(VirtualKey::kMenu, true, VirtualKey::kLMenu);
  CHECK(keyboard.Buttons() == X_INPUT_GAMEPAD_BACK);
  // The left Alt held: the frets as without it.
  keyboard.Key(VirtualKey::kD, true);
  CHECK(keyboard.Buttons() == (X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_A));
  keyboard.Key(VirtualKey::kMenu, false, VirtualKey::kLMenu);
  CHECK(keyboard.Buttons() == X_INPUT_GAMEPAD_A);
  // The right Alt is still a modifier: D alone is silent while it is held.
  keyboard.Key(VirtualKey::kMenu, true, VirtualKey::kRMenu);
  CHECK(keyboard.Buttons() == 0);
  keyboard.Key(VirtualKey::kMenu, false, VirtualKey::kRMenu);
  CHECK(keyboard.Buttons() == X_INPUT_GAMEPAD_A);
}

TEST_CASE("A modifier bound alone is no longer a prefix", "[input][mnk]") {
  Keyboard keyboard;
  keyboard.Key(VirtualKey::kShift, true, VirtualKey::kLShift);
  keyboard.Key(VirtualKey::kLeft, true);
  CHECK(keyboard.Buttons() == X_INPUT_GAMEPAD_DPAD_LEFT);
  keyboard.Key(VirtualKey::kLeft, false);
  keyboard.Key(VirtualKey::kShift, false, VirtualKey::kLShift);

  // "Shift": either side is the control's key, so "Shift+Left" never fires.
  REXCVAR_SET(keybind_b, "Shift");
  keyboard.Key(VirtualKey::kShift, true, VirtualKey::kLShift);
  keyboard.Key(VirtualKey::kLeft, true);
  CHECK(keyboard.Buttons() == X_INPUT_GAMEPAD_B);
  keyboard.Key(VirtualKey::kLeft, false);
  // Both sides down, one let go: still held.
  keyboard.Key(VirtualKey::kShift, true, VirtualKey::kRShift);
  keyboard.Key(VirtualKey::kShift, false, VirtualKey::kLShift);
  CHECK(keyboard.Buttons() == X_INPUT_GAMEPAD_B);
  keyboard.Key(VirtualKey::kShift, false, VirtualKey::kRShift);
  CHECK(keyboard.Buttons() == 0);
}
