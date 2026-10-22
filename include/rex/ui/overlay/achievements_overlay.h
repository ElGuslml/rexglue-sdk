/**
 * @file        rex/ui/overlay/achievements_overlay.h
 * @brief       Default ImGui achievements overlay dialog.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The recomp branch's design (RECOMP.md): a panel at the center of the screen with the totals, the
 *              progress and every achievement (its icon dimmed while locked, its unlock date), in the dialogs'
 *              language and style (OverlayLanguage(), DialogStyle).
 */
#pragma once

#include <cstdint>
#include <vector>

#include <rex/system/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;

class AchievementsOverlayDialog : public ImGuiDialog {
 public:
  // `immediate_drawer` is not used: the icons are made with the ImGuiDrawer's current drawer
  // (AchievementIconCache). The parameter stays for the callers of upstream's constructor.
  AchievementsOverlayDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                            rex::Runtime* runtime, rex::system::AchievementManager* achievements);
  ~AchievementsOverlayDialog() override;

  // Drawn with the game's frames: the list changes only when an achievement unlocks (asking for continuous
  // painting made the Xenos presenter repaint at the host's refresh rate, 220 FPS, while it was open).
  bool WantsContinuousPaint() override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Lazily loads icon_path, or icons/<image_id>.png when icon_path is empty.
  ImmediateTexture* GetIcon(const rex::system::AchievementInfo& achievement);

  rex::Runtime* runtime_ = nullptr;
  rex::system::AchievementManager* achievements_ = nullptr;
  AchievementIconCache icon_cache_;
  // The achievements in the dialogs' language, made again when the language or the list changes.
  std::vector<rex::system::AchievementInfo> localized_;
  uint32_t localized_language_ = 0;
};

}  // namespace rex::ui
