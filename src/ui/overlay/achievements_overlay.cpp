/**
 * @file        ui/overlay/achievements_overlay.cpp
 * @brief       Achievements overlay implementation. See achievements_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievements_overlay.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/overlay_language.h>

#include "ui/overlay/achievement_ui.h"
#include "ui/overlay/dialog_draw.h"

namespace rex::ui {

AchievementsOverlayDialog::AchievementsOverlayDialog(ImGuiDrawer* imgui_drawer,
                                                     ImmediateDrawer* immediate_drawer,
                                                     rex::Runtime* runtime,
                                                     rex::system::AchievementManager* achievements)
    : ImGuiDialog(imgui_drawer),
      runtime_(runtime),
      achievements_(achievements),
      icon_cache_(imgui_drawer, runtime) {
  (void)immediate_drawer;
}

AchievementsOverlayDialog::~AchievementsOverlayDialog() {}

ImmediateTexture* AchievementsOverlayDialog::GetIcon(
    const rex::system::AchievementInfo& achievement) {
  return icon_cache_.GetIcon(achievement);
}

namespace {

// A check mark in a `size` square at `min`.
void DrawCheck(const dialog::Painter& p, ImVec2 min, float size, const ImVec4& color) {
  ImVec2 points[3] = {ImVec2(min.x + size * 0.08f, min.y + size * 0.55f),
                      ImVec2(min.x + size * 0.38f, min.y + size * 0.84f),
                      ImVec2(min.x + size * 0.94f, min.y + size * 0.18f)};
  p.list()->AddPolyline(points, 3, p.Color(color), ImDrawFlags_None, std::max(1.0f, size * 0.16f));
}

}  // namespace

void AchievementsOverlayDialog::OnDraw(ImGuiIO& io) {
  uint32_t language = OverlayLanguage();
  const AchievementTexts& texts = AchievementTextsFor(language);
  const auto achievements = achievements_->ListAchievements();
  if (language != localized_language_ || localized_.size() != achievements.size()) {
    localized_.clear();
    for (const auto& achievement : achievements) {
      localized_.push_back(LocalizeAchievement(runtime_, achievement, language));
    }
    localized_language_ = language;
  }

  int unlocked_count = 0;
  int total_gs = 0;
  int earned_gs = 0;
  for (const auto& a : achievements) {
    total_gs += static_cast<int>(a.gamerscore);
    if (achievements_->IsUnlocked(a.id)) {
      ++unlocked_count;
      earned_gs += static_cast<int>(a.gamerscore);
    }
  }
  const int total_count = static_cast<int>(achievements.size());

  using dialog::Font;
  float scale = dialog::Painter::ScaleFor(io);
  dialog::Painter back(imgui_drawer(), ImGui::GetBackgroundDrawList(), scale);
  const DialogStyle& s = back.style();

  // The panel, in pixels of a 720-line screen.
  constexpr float kPad = 24.0f, kRow = 76.0f, kIcon = 52.0f, kRight = 130.0f;
  float width = std::min(back.Px(760.0f), io.DisplaySize.x - back.Px(48.0f));
  float height = std::min(back.Px(640.0f), io.DisplaySize.y - back.Px(64.0f));
  ImVec2 min(std::floor((io.DisplaySize.x - width) * 0.5f), std::floor((io.DisplaySize.y - height) * 0.5f));
  ImVec2 max(min.x + width, min.y + height);

  // The game dimmed behind it, the card.
  back.list()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, back.Color(s.dim_background, 0.75f));
  back.Shadow(min, max, s.rounding);
  back.Rect(min, max, s.background, s.rounding);
  back.Border(min, max, s.border, s.rounding);

  ImGui::SetNextWindowPos(min, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, back.Px(6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, back.Px(3.0f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, s.raised);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, s.dim);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, s.muted);
  if (ImGui::Begin("##achievements_overlay", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav)) {
    dialog::Painter p(imgui_drawer(), ImGui::GetWindowDrawList(), scale);

    // ---- Header: the title, the key that closes the list, the totals and the progress ----------------------
    float x = min.x + p.Px(kPad);
    float right = max.x - p.Px(kPad);
    float y = min.y + p.Px(kPad - 4.0f);
    constexpr float kTitleEm = 26.0f;
    p.Text(Font::kCondensedBold, kTitleEm, ImVec2(x, y), s.text, texts.title);
    std::string key = rex::cvar::GetFlagByName("bind_achievements");
    char hint[96];
    std::snprintf(hint, sizeof(hint), texts.close_hint, key.empty() ? "F7" : key.c_str());
    float hint_width = p.Measure(Font::kRegular, 12.0f, hint);
    p.Text(Font::kRegular, 12.0f,
           ImVec2(right - hint_width, y + (p.LineHeight(kTitleEm) - p.LineHeight(12.0f)) * 0.5f), s.dim, hint);
    y += p.LineHeight(kTitleEm) + p.Px(10.0f);

    // "7 / 30", "175 / 1000 G", then the progress bar and its percentage.
    constexpr float kBigEm = 22.0f, kSmallEm = 16.0f;
    float baseline_offset = p.LineHeight(kBigEm) - p.LineHeight(kSmallEm) - p.Px(1.5f);
    auto pair = [&](float at, const std::string& big, const std::string& rest, const ImVec4& color) {
      p.Text(Font::kCondensedBold, kBigEm, ImVec2(at, y), color, big);
      at += p.Measure(Font::kCondensedBold, kBigEm, big) + p.Px(4.0f);
      p.Text(Font::kCondensedBold, kSmallEm, ImVec2(at, y + baseline_offset), s.dim, rest);
      return at + p.Measure(Font::kCondensedBold, kSmallEm, rest);
    };
    float at = pair(x, std::to_string(unlocked_count), "/ " + std::to_string(total_count), s.text);
    at = pair(at + p.Px(28.0f), std::to_string(earned_gs), "/ " + std::to_string(total_gs) + " G", s.accent);
    int percent = total_count > 0 ? int(std::lround(100.0 * unlocked_count / total_count)) : 0;
    std::string percent_text = std::to_string(percent) + " %";
    float percent_width = p.Measure(Font::kSemiBold, 13.0f, percent_text);
    p.Text(Font::kSemiBold, 13.0f,
           ImVec2(right - percent_width, y + (p.LineHeight(kBigEm) - p.LineHeight(13.0f)) * 0.5f), s.muted,
           percent_text);
    float bar_left = at + p.Px(28.0f);
    float bar_right = right - percent_width - p.Px(12.0f);
    if (bar_right - bar_left > p.Px(40.0f)) {
      float bar_y = y + (p.LineHeight(kBigEm) - p.Px(6.0f)) * 0.5f;
      p.Rect(ImVec2(bar_left, bar_y), ImVec2(bar_right, bar_y + p.Px(6.0f)), s.raised, 3.0f);
      if (unlocked_count > 0 && total_count > 0) {
        float fill = (bar_right - bar_left) * float(unlocked_count) / float(total_count);
        p.Rect(ImVec2(bar_left, bar_y), ImVec2(bar_left + std::max(fill, p.Px(6.0f)), bar_y + p.Px(6.0f)), s.accent,
               3.0f);
      }
    }
    y += p.LineHeight(kBigEm) + p.Px(16.0f);
    p.list()->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), p.Color(s.border), std::max(1.0f, p.Px(1.0f)));

    // ---- The list -------------------------------------------------------------------------------------------
    ImVec2 list_min(min.x + p.Px(kPad * 0.5f), y + p.Px(1.0f));
    ImVec2 list_size(width - p.Px(kPad * 0.5f) - p.Px(8.0f), max.y - list_min.y - p.Px(8.0f));
    ImGui::SetCursorScreenPos(list_min);
    if (ImGui::BeginChild("##achievement_list", list_size, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav)) {
      dialog::Painter l(imgui_drawer(), ImGui::GetWindowDrawList(), scale);
      ImVec2 origin = ImGui::GetCursorScreenPos();
      float row_height = l.Px(kRow);
      float row_width = list_size.x - l.Px(kPad * 0.5f + 6.0f);
      float clip_top = list_min.y, clip_bottom = list_min.y + list_size.y;
      for (size_t i = 0; i < localized_.size(); ++i) {
        const auto& a = localized_[i];
        ImVec2 row(origin.x, origin.y + row_height * float(i));
        if (row.y + row_height < clip_top || row.y > clip_bottom) {
          continue;
        }
        const bool unlocked = achievements_->IsUnlocked(a.id);
        const bool hidden = !unlocked && IsSecretAchievement(a);
        if (i > 0) {
          l.list()->AddLine(ImVec2(row.x + l.Px(kPad * 0.5f), row.y), ImVec2(row.x + row_width, row.y),
                            l.Color(s.border), std::max(1.0f, l.Px(1.0f)));
        }

        // The icon, dimmed while locked.
        ImVec2 icon_min(row.x + l.Px(kPad * 0.5f), row.y + (row_height - l.Px(kIcon)) * 0.5f);
        ImVec2 icon_max(icon_min.x + l.Px(kIcon), icon_min.y + l.Px(kIcon));
        ImmediateTexture* icon = hidden ? nullptr : GetIcon(a);
        if (icon) {
          ImVec4 tint = unlocked ? ImVec4(1, 1, 1, 1) : ImVec4(0.38f, 0.38f, 0.40f, 1.0f);
          l.list()->AddImageRounded(reinterpret_cast<ImTextureID>(icon), icon_min, icon_max, ImVec2(0, 0),
                                    ImVec2(1, 1), l.Color(tint), l.Px(8.0f));
        } else {
          l.Rect(icon_min, icon_max, s.raised, 8.0f);
          float mark = l.Measure(Font::kCondensedBold, 24.0f, "?");
          l.Text(Font::kCondensedBold, 24.0f,
                 ImVec2(icon_min.x + (l.Px(kIcon) - mark) * 0.5f,
                        icon_min.y + (l.Px(kIcon) - l.LineHeight(24.0f)) * 0.5f),
                 s.dim, "?");
        }
        l.Border(icon_min, icon_max, s.border, 8.0f);

        // The name and description.
        float text_x = icon_max.x + l.Px(16.0f);
        float text_width = row.x + row_width - l.Px(kRight) - text_x;
        constexpr float kNameEm = 15.0f, kTextEm = 12.5f;
        const std::string& name = hidden ? std::string(texts.secret) : a.label;
        const std::string& description =
            hidden ? std::string(texts.secret_hint) : (unlocked ? a.description : a.unachieved_description);
        float description_height =
            l.Paragraph(Font::kRegular, kTextEm, ImVec2(0, 0), text_width, s.muted, description, 2, false);
        float block = l.LineHeight(kNameEm) + l.Px(2.0f) + description_height;
        float text_y = row.y + std::floor((row_height - block) * 0.5f);
        l.Text(Font::kSemiBold, kNameEm, ImVec2(text_x, text_y), unlocked ? s.text : s.muted,
               l.Ellipsize(Font::kSemiBold, kNameEm, name, text_width));
        l.Paragraph(Font::kRegular, kTextEm, ImVec2(text_x, text_y + l.LineHeight(kNameEm) + l.Px(2.0f)), text_width,
                    unlocked ? s.muted : s.dim, description, 2);

        // The gamerscore and the state: the unlock date, or "Locked".
        constexpr float kScoreEm = 17.0f, kStateEm = 12.0f;
        float column_right = row.x + row_width;
        std::string score = std::to_string(a.gamerscore) + " G";
        float score_y = row.y + (row_height - l.LineHeight(kScoreEm) - l.Px(2.0f) - l.LineHeight(kStateEm)) * 0.5f;
        l.Text(Font::kCondensedBold, kScoreEm,
               ImVec2(column_right - l.Measure(Font::kCondensedBold, kScoreEm, score), score_y),
               unlocked ? s.accent : s.dim, score);
        float state_y = score_y + l.LineHeight(kScoreEm) + l.Px(2.0f);
        if (unlocked) {
          std::string date = dialog::FormatDate(achievements_->GetUnlockTime(a.id), language);
          if (date.empty()) {
            date = texts.unlocked;
          }
          float date_width = l.Measure(Font::kSemiBold, kStateEm, date);
          float check = l.Px(10.0f);
          float date_x = column_right - date_width;
          l.Text(Font::kSemiBold, kStateEm, ImVec2(date_x, state_y), s.success, date);
          DrawCheck(l, ImVec2(date_x - check - l.Px(5.0f), state_y + (l.LineHeight(kStateEm) - check) * 0.5f), check,
                    s.success);
        } else {
          l.Text(Font::kSemiBold, kStateEm,
                 ImVec2(column_right - l.Measure(Font::kSemiBold, kStateEm, texts.locked), state_y), s.dim,
                 texts.locked);
        }
      }
      ImGui::Dummy(ImVec2(row_width, row_height * float(localized_.size()) + l.Px(8.0f)));
    }
    ImGui::EndChild();
  }
  ImGui::End();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(3);
}

}  // namespace rex::ui
