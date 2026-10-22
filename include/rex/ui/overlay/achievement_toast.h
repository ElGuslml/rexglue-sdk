/**
 * @file        rex/ui/overlay/achievement_toast.h
 * @brief       Thread-safe achievement unlock toast - a card at the top center of the screen that
 *              slides in and goes after a few seconds.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     The recomp branch's design (RECOMP.md): the achievement's icon, "ACHIEVEMENT UNLOCKED" in the
 *              dialogs' language (OverlayLanguage()), the name and description, the gamerscore in a pill, and a
 *              bar for the time left; Barlow, scaled with the screen's height (DialogStyle).
 */
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;

class AchievementToastDialog : public AchievementNotificationDialog {
 public:
  // `immediate_drawer` is not used: the icons are made with the ImGuiDrawer's current drawer
  // (AchievementIconCache). The parameter stays for the callers of upstream's constructor.
  AchievementToastDialog(ImGuiDrawer* drawer, ImmediateDrawer* immediate_drawer,
                         rex::Runtime* runtime);
  ~AchievementToastDialog() override;

  // Thread-safe: safe to call from any thread, including guest threads.
  void Push(const rex::system::AchievementEvent& event) override;

  // Only while a toast is queued or showing (it is registered all session).
  bool WantsContinuousPaint() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr float kDisplaySeconds = 6.0f;
  static constexpr float kEnterSeconds = 0.4f;
  static constexpr float kExitSeconds = 0.45f;

  struct PendingToast {
    rex::system::AchievementEvent event;
    std::chrono::steady_clock::time_point arrived;
    std::optional<std::chrono::steady_clock::time_point> visible_since;
    bool localized = false;  // event.achievement's texts are in the dialogs' language
  };

  rex::Runtime* runtime_ = nullptr;
  std::mutex mutex_;
  std::deque<PendingToast> queue_;
  AchievementIconCache icon_cache_;
};

}  // namespace rex::ui
