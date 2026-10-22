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
// one. The game side says when it shows a screen the player can quit from, in one of two ways:
// - OfferQuitPrompt() on every frame of that screen (a [[midasm_hook]] in the code that runs it; KameoRecomp);
// - SetQuitPromptCondition() once, with a test of the game's state (what its hooks recorded, or guest memory)
//   that the filter runs when a press of B begins: for a game whose screens change on events rather than in code
//   that runs every frame (POTCAWERecomp, its menus).
// A press of B that begins while it is offered opens a dialog, "Are you sure you want to quit the game?" in the
// game's language (user_language), which A or Enter accepts (the window closes, as with its close button) and B
// or Escape cancels; the mouse works too.
//
// The game never sees those presses: while the dialog is open, and on each controller until all its buttons are
// released after the dialog closes, the guest reads the controller at rest and gets no keystrokes
// (QuitPromptInputFilter, installed with InputSystem::SetHostInputFilter). The controller is read when the
// guest reads it, so the dialog answers as long as the game keeps polling (games poll every frame).
//
// --quit_prompt=false turns it off: the dialog never opens then.

// Called by ReXApp once the ImGui drawer exists. `accept` runs on the UI thread when the player confirms.
void SetupQuitPrompt(ImGuiDrawer* imgui_drawer, WindowedAppContext* app_context,
                     std::function<void()> accept);

// The game shows a screen the player can quit from in this frame. Any thread; cheap.
void OfferQuitPrompt();

// The game shows a screen the player can quit from whenever `condition` returns true. It runs on the guest's
// thread that reads the controller, when a press of B begins (not every frame), so it can read the game's state;
// it must not call back into this API. An empty function removes it. Works together with OfferQuitPrompt().
void SetQuitPromptCondition(std::function<bool()> condition);

// The dialog's language (an XLanguage, as user_language): for a game that asks for its language in its own menu
// instead of following user_language (POTCAWERecomp passes the Windows display language). 0 = user_language.
void SetQuitPromptLanguage(uint32_t xlanguage);

// Whether the dialog is open.
bool IsQuitPromptOpen();

// The input filter (InputSystem::HostInputFilter): watches each user's real gamepad whenever the guest reads it,
// opens the dialog on a new press of B while the prompt is offered, answers A and B while it is open, and returns
// true while the guest must not see the controller.
bool QuitPromptInputFilter(uint32_t user_index, const rex::input::X_INPUT_GAMEPAD& gamepad);

}  // namespace rex::ui
