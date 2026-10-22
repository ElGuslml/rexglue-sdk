/**
 * @file        ui/overlay/achievement_ui.cpp
 * @brief       Texts of the achievement toast and list (see achievement_ui.h)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include "ui/overlay/achievement_ui.h"

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

namespace rex::ui {

const AchievementTexts& AchievementTextsFor(uint32_t language) {
  static const AchievementTexts kEnglish = {
      "ACHIEVEMENT UNLOCKED", "Achievements", "Unlocked", "Locked", "Secret achievement",
      "Keep playing to discover it.", "%s to close"};
  static const AchievementTexts kJapanese = {
      "実績解除", "実績", "解除済み", "未解除", "シークレット実績", "プレイを続けて見つけよう。", "%sで閉じる"};
  static const AchievementTexts kGerman = {
      "ERFOLG FREIGESCHALTET", "Erfolge", "Freigeschaltet", "Gesperrt", "Geheimer Erfolg",
      "Spiel weiter, um ihn zu entdecken.", "%s zum Schließen"};
  static const AchievementTexts kFrench = {
      "SUCCÈS DÉBLOQUÉ", "Succès", "Débloqué", "Verrouillé", "Succès secret",
      "Continuez à jouer pour le découvrir.", "%s pour fermer"};
  static const AchievementTexts kSpanish = {
      "LOGRO DESBLOQUEADO", "Logros", "Desbloqueado", "Bloqueado", "Logro secreto",
      "Sigue jugando para descubrirlo.", "%s para cerrar"};
  static const AchievementTexts kItalian = {
      "OBIETTIVO SBLOCCATO", "Obiettivi", "Sbloccato", "Bloccato", "Obiettivo segreto",
      "Continua a giocare per scoprirlo.", "%s per chiudere"};
  static const AchievementTexts kPortuguese = {
      "CONQUISTA DESBLOQUEADA", "Conquistas", "Desbloqueada", "Bloqueada", "Conquista secreta",
      "Continue jogando para descobri-la.", "%s para fechar"};
  switch (language) {
    case 2:
      return kJapanese;
    case 3:
      return kGerman;
    case 4:
      return kFrench;
    case 5:
      return kSpanish;
    case 6:
      return kItalian;
    case 9:
      return kPortuguese;
    default:
      return kEnglish;
  }
}

rex::system::AchievementInfo LocalizeAchievement(rex::Runtime* runtime, const rex::system::AchievementInfo& info,
                                                 uint32_t language) {
  using rex::system::XLanguage;
  if (!runtime || !runtime->kernel_state()) {
    return info;
  }
  const auto db = runtime->kernel_state()->title_xdbf();
  if (!db.is_valid()) {
    return info;
  }
  const auto requested = static_cast<XLanguage>(language);
  const XLanguage loaded = db.GetExistingLanguage(static_cast<XLanguage>(REXCVAR_GET(user_language)));
  if (db.GetExistingLanguage(requested) != requested || requested == loaded) {
    return info;
  }
  for (const auto& entry : db.GetAchievements()) {
    if (entry.id != info.id) {
      continue;
    }
    if (db.GetStringTableEntry(loaded, entry.label_id) != info.label) {
      return info;  // achievements.toml gave its own texts
    }
    rex::system::AchievementInfo result = info;
    result.label = db.GetStringTableEntry(requested, entry.label_id);
    result.description = db.GetStringTableEntry(requested, entry.description_id);
    result.unachieved_description = db.GetStringTableEntry(requested, entry.unachieved_id);
    return result;
  }
  return info;
}

}  // namespace rex::ui
