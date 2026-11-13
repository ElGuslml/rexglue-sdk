/**
 * ReXGlue online play: the notification for an invite received from a friend's game.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <rex/input/input.h>
#include <rex/ui/overlay/quit_prompt.h>

namespace rex::ui {

// A friend invites this game to its party (online play, rex/system/online/session_service.h): a card at the top of
// the screen, as an achievement's, says "<name> invites you to play" in the game's language (OverlayLanguage()) for
// kInviteShowSeconds, with a bar for the time left. Holding START (or Enter) for kInviteHoldSeconds joins; let it go
// by and it is ignored. Nothing is blocked: the game plays on under it, except START, which the card takes while it
// is shown (InvitePromptInputFilter), so that holding it does not also pause the game.
//
// A newer invite replaces one not answered yet (the older is ignored). Accepted, the session service tells the game
// once the game allows (SessionService::SetInviteGate).
constexpr float kInviteShowSeconds = 15.0f;
constexpr float kInviteHoldSeconds = 2.0f;

// Called by ReXApp once the ImGui drawer exists.
void SetupInvitePrompt(ImGuiDrawer* imgui_drawer, WindowedAppContext* app_context, QuitPromptInput input);

// An invite to show: `answer` is called once, on the UI thread, with the player's choice. Any thread.
void ShowInvitePrompt(const std::string& inviter, std::function<void(bool accept)> answer);

// When the card may appear (asked every 250 ms while an invite waits); none: at once.
void SetInvitePromptCondition(std::function<bool()> condition);

bool IsInvitePromptOpen();

// The host input filter's part (InputSystem::SetHostInputFilter, with QuitPromptInputFilter): while the card is
// shown it takes the reads in which START is held and counts how long it has been held.
bool InvitePromptInputFilter(uint32_t user_index, const rex::input::X_INPUT_GAMEPAD& gamepad);

}  // namespace rex::ui
