#pragma once
/**
 * @file        rex/ui/overlay/overlay_language.h
 * @brief       The language of the SDK's dialogs: the quit prompt, the achievement toast and list
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <cstdint>

namespace rex::ui {

// The dialogs follow user_language, like the game's own texts. A game that asks for its language in its own menu
// instead of following user_language sets theirs (POTCAWERecomp passes the Windows display language). An XLanguage;
// 0 = user_language. Any thread.
void SetOverlayLanguage(uint32_t xlanguage);

// The language the dialogs draw in: what SetOverlayLanguage set, or user_language.
uint32_t OverlayLanguage();

}  // namespace rex::ui
