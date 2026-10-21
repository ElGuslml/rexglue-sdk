#pragma once
/**
 * @file        rex/ui/overlay/quit_prompt.h
 * @brief       A confirmation before quitting, offered by the game from the screens it can be quit from
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <cstdint>
#include <functional>

#include <rex/input/input.h>

namespace rex::ui {

class ImGuiDrawer;
class WindowedAppContext;

// Console games have no way out of themselves: on PC, B on the title screen ("PRESS START") is the natural
// one. The game side calls OfferQuitPrompt() on every frame it shows a screen the player can quit from (a
// [[midasm_hook]] in the code that runs that screen). A press of B that begins while it is offered opens a
// dialog, "Are you sure you want to quit the game?" in the game's language (user_language), which A or Enter
// accepts (the window closes, as with its close button) and B or Escape cancels; the mouse works too.
//
// The game never sees those presses. The press of B that opens the dialog is taken from the guest's read
// (QuitPromptInputFilter, installed with InputSystem::SetHostInputFilter); while the dialog is open it holds an
// input blocker, so the guest reads the pad at rest and gets no keystrokes, and it reads the controller itself
// (InputSystem::GetStateForUI) on every frame it is drawn. The buttons held when it closes stay masked until
// released, so the press that closed it does not reach the game either.
//
// --quit_prompt=false turns it off: OfferQuitPrompt() does nothing then.

// How the dialog reaches the controller: ReXApp passes the InputSystem's blocker and its read for the UI (rexui
// cannot use the InputSystem itself, since rexinput links rexui).
struct QuitPromptInput {
  // true: AddUIInputBlocker(); false: RemoveUIInputBlocker().
  std::function<void(bool)> block;
  // GetStateForUI(user_index): false when no controller answers for that user.
  std::function<bool(uint32_t user_index, rex::input::X_INPUT_GAMEPAD& gamepad)> read;
};

// Called by ReXApp once the ImGui drawer and the input system exist. `accept` runs on the UI thread when the
// player confirms.
void SetupQuitPrompt(ImGuiDrawer* imgui_drawer, WindowedAppContext* app_context, QuitPromptInput input,
                     std::function<void()> accept);

// The game shows a screen the player can quit from in this frame. Any thread; cheap.
void OfferQuitPrompt();

// Whether the dialog is open.
bool IsQuitPromptOpen();

// The input filter (InputSystem::HostInputFilter): watches each user's pad whenever the guest reads it, and takes
// a new press of B while the prompt is offered (returns true), opening the dialog.
bool QuitPromptInputFilter(uint32_t user_index, const rex::input::X_INPUT_GAMEPAD& gamepad);

}  // namespace rex::ui
