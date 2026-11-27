#pragma once
/**
 * @file        rex/ui/overlay/keyboard_prompt.h
 * @brief       An on-screen keyboard for the text a game asks the console for (XamShowKeyboardUI)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <rex/input/input.h>

struct ImGuiIO;

namespace rex::ui {

class ImGuiDrawer;

// A game asks the console for a text (a character's or a band's name) with XamShowKeyboardUI, and the console shows
// its keyboard, typed on with the controller. This is that keyboard: a card in the recomp branch's dialog style
// (DialogStyle, scaled with the screen's height) with the game's title and description, the text with its caret, and
// rows of keys in three sets (letters, capitals, symbols and accented letters).
//
// - The controller: the D-pad or the left stick moves over the keys, A types the one under the cursor, X erases,
//   Y is a space, LB and RB move the caret, LT is shift, RT changes to the symbols, Start accepts, B cancels. Every
//   action is also a key of the last row, so a controller with only a D-pad and one button (a guitar) can do all.
// - The keyboard: typed as text; Backspace, Delete, the arrows, Home and End edit it, Enter accepts, Escape cancels.
// - The mouse: a click on a key.
//
// The dialog that owns it (xam_ui.cpp's, which holds the guest's input blocked) calls Draw() in every frame until it
// returns something other than kOpen.
struct KeyboardPromptParams {
  std::string title;        // UTF-8, as the two below.
  std::string description;
  std::string text;         // The text it starts with.
  // The UTF-16 code units the game's buffer takes without its terminator; 0: no limit.
  size_t max_units = 0;
  // A user's controllers, without the keyboard's pad (InputSystem::GetControllersForUI): false when none answers.
  // Empty: keyboard and mouse only.
  std::function<bool(uint32_t user_index, rex::input::X_INPUT_GAMEPAD& gamepad)> read;
};

class KeyboardPrompt {
 public:
  enum class Result { kOpen, kAccepted, kCancelled };

  KeyboardPrompt(ImGuiDrawer* imgui_drawer, KeyboardPromptParams params);
  ~KeyboardPrompt();

  // UI thread, once a frame: reads the controller, the keyboard and the mouse, and draws.
  Result Draw(ImGuiIO& io);

  // The text as it is now, UTF-8.
  std::string text() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::ui
