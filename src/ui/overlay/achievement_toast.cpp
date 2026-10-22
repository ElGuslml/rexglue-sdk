/**
 * @file        ui/overlay/achievement_toast.cpp
 * @brief       Achievement toast implementation. See achievement_toast.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievement_toast.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/overlay_language.h>

#include "ui/overlay/achievement_ui.h"
#include "ui/overlay/dialog_draw.h"

namespace rex::ui {

AchievementToastDialog::AchievementToastDialog(ImGuiDrawer* drawer,
                                               ImmediateDrawer* immediate_drawer,
                                               rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), runtime_(runtime), icon_cache_(drawer, runtime) {
  (void)immediate_drawer;
}

AchievementToastDialog::~AchievementToastDialog() {}

void AchievementToastDialog::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({event, std::chrono::steady_clock::now()});
}

bool AchievementToastDialog::WantsContinuousPaint() {
  // A new toast is picked up by the next paint caused by a guest frame.
  std::lock_guard<std::mutex> lock(mutex_);
  return !queue_.empty();
}

void AchievementToastDialog::OnDraw(ImGuiIO& io) {
  auto now = std::chrono::steady_clock::now();
  uint32_t language = OverlayLanguage();

  PendingToast toast;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      if (!queue_.front().visible_since) {
        queue_.front().visible_since = now;
      }
      age = std::chrono::duration<float>(now - *queue_.front().visible_since).count();
      if (age >= kDisplaySeconds) {
        queue_.pop_front();
      } else {
        break;
      }
    }
    if (queue_.empty()) {
      return;
    }
    if (!queue_.front().localized) {
      queue_.front().event.achievement = LocalizeAchievement(runtime_, queue_.front().event.achievement, language);
      queue_.front().localized = true;
    }
    toast = queue_.front();
  }
  const rex::system::AchievementInfo& info = toast.event.achievement;
  const AchievementTexts& texts = AchievementTextsFor(language);

  // In: slides down from above the screen and fades in. Out: fades while rising a little.
  float enter = std::min(1.0f, age / kEnterSeconds);
  enter = 1.0f - (1.0f - enter) * (1.0f - enter) * (1.0f - enter);  // ease out
  float alpha = std::min(1.0f, age / (kEnterSeconds * 0.6f));
  float exit = std::clamp((age - (kDisplaySeconds - kExitSeconds)) / kExitSeconds, 0.0f, 1.0f);
  alpha *= 1.0f - exit;
  // The bar: the time left, from the end of the entry to the start of the exit.
  float left = 1.0f - std::clamp((age - kEnterSeconds) / (kDisplaySeconds - kEnterSeconds - kExitSeconds), 0.0f, 1.0f);

  dialog::Painter p(imgui_drawer(), ImGui::GetForegroundDrawList(), dialog::Painter::ScaleFor(io), alpha);
  const DialogStyle& s = p.style();
  using dialog::Font;

  // The layout, in pixels of a 720-line screen.
  constexpr float kMargin = 20.0f, kPad = 14.0f, kIcon = 56.0f, kGap = 14.0f, kBar = 3.0f;
  constexpr float kHeaderEm = 11.0f, kNameEm = 15.5f, kTextEm = 12.5f, kPillEm = 12.0f;
  float width = std::min(p.Px(420.0f), io.DisplaySize.x - p.Px(32.0f));
  float text_x = p.Px(kPad + kIcon + kGap);
  float text_width = width - text_x - p.Px(kPad);

  // The gamerscore pill, on the header's line.
  std::string score = std::to_string(info.gamerscore) + "G";
  float pill_height = p.Px(18.0f);
  float pill_width = p.Measure(Font::kCondensedBold, kPillEm, score) + p.Px(16.0f);
  float header_width = text_width - pill_width - p.Px(8.0f);

  float header_height = std::max(p.LineHeight(kHeaderEm), pill_height);
  float name_top = header_height + p.Px(3.0f);
  float description_top = name_top + p.LineHeight(kNameEm) + p.Px(2.0f);
  float description_height =
      p.Paragraph(Font::kRegular, kTextEm, ImVec2(0, 0), text_width, s.muted, info.description, 2, false);
  float content_height = std::max(p.Px(kIcon), description_top + description_height);
  float height = p.Px(kPad) * 2.0f + content_height + p.Px(kBar + 6.0f);

  float x0 = std::floor((io.DisplaySize.x - width) * 0.5f);
  float y0 = std::floor(p.Px(kMargin) - (1.0f - enter) * (height + p.Px(kMargin + 12.0f)) - exit * p.Px(10.0f));
  ImVec2 min(x0, y0), max(x0 + width, y0 + height);

  p.Shadow(min, max, s.rounding);
  p.Rect(min, max, s.background, s.rounding);
  p.Border(min, max, s.border, s.rounding);

  // The icon, or the gamerscore in its place.
  ImVec2 icon_min(x0 + p.Px(kPad), y0 + p.Px(kPad) + std::floor((content_height - p.Px(kIcon)) * 0.5f));
  ImVec2 icon_max(icon_min.x + p.Px(kIcon), icon_min.y + p.Px(kIcon));
  if (ImmediateTexture* icon = icon_cache_.GetIcon(info)) {
    p.list()->AddImageRounded(reinterpret_cast<ImTextureID>(icon), icon_min, icon_max, ImVec2(0, 0), ImVec2(1, 1),
                              p.Color(ImVec4(1, 1, 1, 1)), p.Px(8.0f));
  } else {
    p.Rect(icon_min, icon_max, s.raised, 8.0f);
    float score_width = p.Measure(Font::kCondensedBold, 22.0f, score);
    p.Text(Font::kCondensedBold, 22.0f,
           ImVec2(icon_min.x + (p.Px(kIcon) - score_width) * 0.5f,
                  icon_min.y + (p.Px(kIcon) - p.LineHeight(22.0f)) * 0.5f),
           s.accent, score);
  }
  p.Border(icon_min, icon_max, s.border, 8.0f);

  // The texts.
  float tx = x0 + text_x;
  float ty = y0 + p.Px(kPad);
  std::string header = p.Ellipsize(Font::kCondensedBold, kHeaderEm, texts.unlocked_header, header_width);
  p.Text(Font::kCondensedBold, kHeaderEm, ImVec2(tx, ty + (header_height - p.LineHeight(kHeaderEm)) * 0.5f), s.accent,
         header, 0.14f);
  ImVec2 pill_min(x0 + width - p.Px(kPad) - pill_width, ty + (header_height - pill_height) * 0.5f);
  ImVec2 pill_max(pill_min.x + pill_width, pill_min.y + pill_height);
  p.Rect(pill_min, pill_max, s.accent, 9.0f);
  p.Text(Font::kCondensedBold, kPillEm,
         ImVec2(pill_min.x + p.Px(8.0f), pill_min.y + (pill_height - p.LineHeight(kPillEm)) * 0.5f), s.on_accent,
         score);
  p.Text(Font::kSemiBold, kNameEm, ImVec2(tx, ty + name_top), s.text,
         p.Ellipsize(Font::kSemiBold, kNameEm, info.label, text_width));
  p.Paragraph(Font::kRegular, kTextEm, ImVec2(tx, ty + description_top), text_width, s.muted, info.description, 2);

  // The time left.
  ImVec2 bar_min(x0 + p.Px(kPad), y0 + height - p.Px(kPad * 0.5f + kBar));
  ImVec2 bar_max(x0 + width - p.Px(kPad), bar_min.y + p.Px(kBar));
  p.Rect(bar_min, bar_max, s.raised, kBar * 0.5f);
  if (left > 0.0f) {
    p.Rect(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * left, bar_max.y), s.accent, kBar * 0.5f);
  }
}

}  // namespace rex::ui
