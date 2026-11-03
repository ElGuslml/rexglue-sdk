/**
 * @file        input/mnk/system_shortcuts.cpp
 * @brief       The system's shortcuts a modifier key playing a control would set off. See system_shortcuts.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include "system_shortcuts.h"

#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include <SDL3/SDL_hints.h>

#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rex::input::mnk {

namespace {

std::mutex shortcuts_mutex;
bool shift_held = false;
bool alt_held = false;

#ifdef _WIN32
// The settings as they were when they were taken, given back as they were.
STICKYKEYS saved_sticky = {sizeof(STICKYKEYS), 0};
FILTERKEYS saved_filter = {sizeof(FILTERKEYS), 0, 0, 0, 0, 0};

void HoldAccessibilityShortcuts(bool hold) {
  if (hold) {
    SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(saved_sticky), &saved_sticky, 0);
    SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof(saved_filter), &saved_filter, 0);
    // Not while the feature is on: its user needs it.
    if (!(saved_sticky.dwFlags & SKF_STICKYKEYSON)) {
      STICKYKEYS off = saved_sticky;
      off.dwFlags &= ~(SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY);
      SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof(off), &off, 0);
    }
    if (!(saved_filter.dwFlags & FKF_FILTERKEYSON)) {
      FILTERKEYS off = saved_filter;
      off.dwFlags &= ~(FKF_HOTKEYACTIVE | FKF_CONFIRMHOTKEY);
      SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof(off), &off, 0);
    }
  } else {
    SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof(saved_sticky), &saved_sticky, 0);
    SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof(saved_filter), &saved_filter, 0);
  }
}
#endif

}  // namespace

void HoldSystemShortcuts(bool shift, bool alt) {
  std::lock_guard lock(shortcuts_mutex);
  if (shift != shift_held) {
    shift_held = shift;
#ifdef _WIN32
    HoldAccessibilityShortcuts(shift);
    if (shift) {
      REXLOG_INFO("Keyboard: a Shift key plays a control: Windows' Sticky Keys and Filter Keys shortcuts held off");
    } else {
      REXLOG_INFO("Keyboard: Windows' Sticky Keys and Filter Keys shortcuts given back");
    }
#endif
  }
  if (alt != alt_held) {
    alt_held = alt;
    if (alt) {
      SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, "0");
      REXLOG_INFO("Keyboard: an Alt key plays a control: Alt+F4 does not close the window");
    } else {
      SDL_ResetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4);
      REXLOG_INFO("Keyboard: Alt+F4 closes the window again");
    }
  }
}

void ReleaseSystemShortcuts() {
  HoldSystemShortcuts(false, false);
}

}  // namespace rex::input::mnk
