/**
 * ReXGlue online play: the prompt for an invite received from a friend's game.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <rex/ui/overlay/quit_prompt.h>

namespace rex::ui {

// A friend invites this game to its party (online play, rex/system/online/session_service.h): a
// dialog says "<name> invites you to play" in the game's language (OverlayLanguage()), A or Enter
// joins, B or Escape ignores; the mouse works too. While it is shown it holds an input blocker
// (the game reads the pad at rest), as the quit prompt does.
//
// It is shown when the game allows (SetInvitePromptCondition: Guitar Hero, not while a song is
// played); until then it waits, unseen. A newer invite replaces one not answered yet (the older is
// ignored).

// Called by ReXApp once the ImGui drawer and the input system exist (the quit prompt's input).
void SetupInvitePrompt(ImGuiDrawer* imgui_drawer, WindowedAppContext* app_context, QuitPromptInput input);

// An invite to ask about: `answer` is called once, on the UI thread, with the player's choice.
// Any thread.
void ShowInvitePrompt(const std::string& inviter, std::function<void(bool accept)> answer);

// When the game may show the dialog (asked on the UI thread each frame while an invite waits); none:
// at once.
void SetInvitePromptCondition(std::function<bool()> condition);

bool IsInvitePromptOpen();

}  // namespace rex::ui
