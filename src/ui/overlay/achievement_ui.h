/**
 * @file        ui/overlay/achievement_ui.h
 * @brief       Texts of the achievement toast and list in the dialogs' language (internal)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */
#pragma once

#include <cstdint>

#include <rex/system/achievement_store.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

struct AchievementTexts {
  const char* unlocked_header;  // the toast's "ACHIEVEMENT UNLOCKED" (uppercase)
  const char* title;            // "Achievements"
  const char* unlocked;         // "Unlocked"
  const char* locked;           // "Locked"
  const char* secret;           // a secret achievement's name while locked
  const char* secret_hint;      // and its description
  const char* close_hint;       // "%s to close" (the key)
};

// By XLanguage (OverlayLanguage()): English, Japanese, German, French, Spanish, Italian and Portuguese; the others
// get English.
const AchievementTexts& AchievementTextsFor(uint32_t language);

// Secret: hidden until unlocked (the SPA's "show unachieved" flag, 0x8, is clear).
inline bool IsSecretAchievement(const rex::system::AchievementInfo& info) {
  return (info.flags & 0x8) == 0;
}

// `info` with its label and descriptions in `language` when the title's XDBF has that language and the texts were
// not overridden (achievements.toml): the achievements are loaded in user_language, and a game may draw the
// dialogs in another (SetOverlayLanguage).
rex::system::AchievementInfo LocalizeAchievement(rex::Runtime* runtime, const rex::system::AchievementInfo& info,
                                                 uint32_t language);

}  // namespace rex::ui
