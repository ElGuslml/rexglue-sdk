/**
 * @file        input/mnk/system_shortcuts.h
 * @brief       The system's shortcuts a modifier key playing a control would set off.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

namespace rex::input::mnk {

/// shift: Windows' accessibility shortcuts that Shift starts are off (Sticky Keys: Shift pressed five times; Filter
/// Keys: the right Shift held eight seconds), as Microsoft advises games ("Disabling Shortcut Keys in Games"), unless
/// the feature itself is on; false gives them back as they were. alt: Alt+F4 does not close the window (SDL's
/// SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4). Acts only on a change; any thread.
void HoldSystemShortcuts(bool shift, bool alt);

}  // namespace rex::input::mnk
