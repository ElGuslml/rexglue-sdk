/**
 * @file        ui/overlay/dialog_draw.h
 * @brief       Drawing for the recomp branch's dialogs: the achievement toast and list, the quit prompt (internal)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <imgui.h>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/style.h>

namespace rex::ui::dialog {

using Font = ImGuiDrawer::Font;

// The dialogs are laid out in pixels of a 720-line screen (DialogStyle's sizes too) and scaled with the screen's
// height, so that they cover the same part of it at any resolution. Text sizes are em sizes, as in CSS.
//
// A Painter draws on a draw list with a scale and a fade (alpha), in Barlow rasterized at the screen's resolution
// (the window's DPI scale as the font density).
class Painter {
 public:
  Painter(ImGuiDrawer* drawer, ImDrawList* list, float scale, float alpha = 1.0f);

  // The scale for a screen: its height over 720 (ImGui units per 720-line pixel), at least 0.5.
  static float ScaleFor(const ImGuiIO& io);

  float scale() const { return scale_; }
  float alpha() const { return alpha_; }
  void set_alpha(float alpha) { alpha_ = alpha; }
  const DialogStyle& style() const { return style_; }
  ImDrawList* list() const { return list_; }

  // 720-line pixels to ImGui units.
  float Px(float px) const { return px * scale_; }
  ImU32 Color(const ImVec4& color, float alpha = 1.0f) const;

  // The height of one line of text (Barlow's line is 1.2 em).
  float LineHeight(float em) const;
  // `tracking` is the letter spacing in em, as CSS's letter-spacing.
  float Measure(Font font, float em, std::string_view text, float tracking = 0.0f) const;
  // Draws one line with its top at `pos`.
  void Text(Font font, float em, ImVec2 pos, const ImVec4& color, std::string_view text, float tracking = 0.0f) const;
  // Wrapped at `width`, at most `max_lines` lines (0: all), the last one ellipsized when cut. Each line is placed
  // at `align` of the room it leaves (0 left, 0.5 centered). Returns the height; draws only when `draw`.
  float Paragraph(Font font, float em, ImVec2 pos, float width, const ImVec4& color, std::string_view text,
                  int max_lines = 0, bool draw = true, float align = 0.0f) const;
  // `text` cut to `width` with an ellipsis.
  std::string Ellipsize(Font font, float em, std::string_view text, float width) const;

  void Rect(ImVec2 min, ImVec2 max, const ImVec4& color, float rounding_px = 0.0f) const;
  void Border(ImVec2 min, ImVec2 max, const ImVec4& color, float rounding_px = 0.0f) const;
  // A soft shadow under a card.
  void Shadow(ImVec2 min, ImVec2 max, float rounding_px) const;

 private:
  ImFont* FontOf(Font font) const;
  float SizeOf(float em) const { return em * 1.2f * scale_; }

  ImGuiDrawer* drawer_;
  ImDrawList* list_;
  float scale_;
  float alpha_;
  const DialogStyle& style_;
};

// "MM/DD/YYYY", "DD.MM.YYYY", "YYYY/MM/DD" or "DD/MM/YYYY" by the language (an XLanguage), for a FILETIME in UTC
// shown in local time.
std::string FormatDate(uint64_t filetime, uint32_t language);

}  // namespace rex::ui::dialog
