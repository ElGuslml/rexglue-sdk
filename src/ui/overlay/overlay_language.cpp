/**
 * @file        ui/overlay/overlay_language.cpp
 * @brief       The language of the SDK's dialogs (see rex/ui/overlay/overlay_language.h)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <rex/ui/overlay/overlay_language.h>

#include <atomic>

#include <rex/cvar.h>
#include <rex/system/flags.h>

namespace rex::ui {
namespace {

std::atomic<uint32_t> g_language{0};  // SetOverlayLanguage(); 0 = user_language

}  // namespace

void SetOverlayLanguage(uint32_t xlanguage) {
  g_language.store(xlanguage, std::memory_order_relaxed);
}

uint32_t OverlayLanguage() {
  uint32_t language = g_language.load(std::memory_order_relaxed);
  return language ? language : REXCVAR_GET(user_language);
}

}  // namespace rex::ui
