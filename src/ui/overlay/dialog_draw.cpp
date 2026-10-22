/**
 * @file        ui/overlay/dialog_draw.cpp
 * @brief       Drawing for the recomp branch's dialogs (see dialog_draw.h)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include "ui/overlay/dialog_draw.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <imgui_internal.h>

#include <rex/platform.h>

namespace rex::ui::dialog {

Painter::Painter(ImGuiDrawer* drawer, ImDrawList* list, float scale, float alpha)
    : drawer_(drawer), list_(list), scale_(scale), alpha_(alpha), style_(drawer->style().dialog) {
  // Rasterized at the screen's pixels: ImGui's units are the window's logical pixels.
  float density = drawer->dpi_scale();
  for (size_t i = 0; i < size_t(Font::kCount); ++i) {
    if (ImFont* font = drawer->font(Font(i))) {
      font->CurrentRasterizerDensity = density;
    }
  }
}

float Painter::ScaleFor(const ImGuiIO& io) {
  return std::max(0.5f, io.DisplaySize.y / 720.0f);
}

ImU32 Painter::Color(const ImVec4& color, float alpha) const {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha * alpha_));
}

ImFont* Painter::FontOf(Font font) const {
  ImFont* result = drawer_->font(font);
  return result ? result : ImGui::GetFont();
}

float Painter::LineHeight(float em) const {
  return SizeOf(em);
}

float Painter::Measure(Font font, float em, std::string_view text, float tracking) const {
  if (text.empty()) {
    return 0.0f;
  }
  float width = FontOf(font)->CalcTextSizeA(SizeOf(em), FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
  if (tracking != 0.0f) {
    int count = 0;
    for (const char* p = text.data(); p < text.data() + text.size();) {
      unsigned int c;
      p += ImTextCharFromUtf8(&c, p, text.data() + text.size());
      ++count;
    }
    width += tracking * em * scale_ * float(std::max(0, count - 1));
  }
  return width;
}

void Painter::Text(Font font, float em, ImVec2 pos, const ImVec4& color, std::string_view text,
                   float tracking) const {
  if (text.empty()) {
    return;
  }
  ImFont* f = FontOf(font);
  float size = SizeOf(em);
  ImU32 col = Color(color);
  const char* end = text.data() + text.size();
  if (tracking == 0.0f) {
    list_->AddText(f, size, pos, col, text.data(), end);
    return;
  }
  // Letter spacing: one character at a time.
  float spacing = tracking * em * scale_;
  for (const char* p = text.data(); p < end;) {
    unsigned int c;
    const char* next = p + ImTextCharFromUtf8(&c, p, end);
    list_->AddText(f, size, pos, col, p, next);
    pos.x += f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, next).x + spacing;
    p = next;
  }
}

float Painter::Paragraph(Font font, float em, ImVec2 pos, float width, const ImVec4& color, std::string_view text,
                         int max_lines, bool draw, float align) const {
  ImFont* f = FontOf(font);
  float size = SizeOf(em);
  float line_height = LineHeight(em);
  const char* p = text.data();
  const char* end = text.data() + text.size();
  int lines = 0;
  while (p < end) {
    const char* line_end = f->CalcWordWrapPosition(size, p, end, width);
    if (line_end <= p) {
      line_end = p + 1;
    }
    // A newline ends the line where it is.
    const char* newline = static_cast<const char*>(std::memchr(p, '\n', size_t(line_end - p)));
    if (newline) {
      line_end = newline;
    }
    const char* next = newline ? newline + 1 : line_end;
    while (next < end && *next == ' ') {
      ++next;
    }
    ++lines;
    bool last = max_lines > 0 && lines == max_lines && next < end;
    if (draw) {
      std::string line = last ? Ellipsize(font, em, std::string_view(p, size_t(end - p)), width)
                              : std::string(p, size_t(line_end - p));
      while (!line.empty() && line.back() == ' ') {
        line.pop_back();
      }
      float room = width - Measure(font, em, line);
      Text(font, em, ImVec2(pos.x + std::max(0.0f, room) * align, pos.y + line_height * float(lines - 1)), color,
           line);
    }
    if (last) {
      break;
    }
    p = next;
  }
  return line_height * float(lines);
}

std::string Painter::Ellipsize(Font font, float em, std::string_view text, float width) const {
  // Only the first line counts.
  if (size_t newline = text.find('\n'); newline != std::string_view::npos) {
    text = text.substr(0, newline);
  }
  if (Measure(font, em, text) <= width) {
    return std::string(text);
  }
  static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026
  // The longest prefix, by whole characters, that fits with the ellipsis.
  std::string best;
  const char* end = text.data() + text.size();
  for (const char* p = text.data(); p < end;) {
    unsigned int c;
    p += ImTextCharFromUtf8(&c, p, end);
    std::string candidate(text.data(), size_t(p - text.data()));
    while (!candidate.empty() && candidate.back() == ' ') {
      candidate.pop_back();
    }
    candidate += kEllipsis;
    if (Measure(font, em, candidate) > width) {
      break;
    }
    best = std::move(candidate);
  }
  return best.empty() ? std::string(kEllipsis) : best;
}

void Painter::Rect(ImVec2 min, ImVec2 max, const ImVec4& color, float rounding_px) const {
  list_->AddRectFilled(min, max, Color(color), Px(rounding_px));
}

void Painter::Border(ImVec2 min, ImVec2 max, const ImVec4& color, float rounding_px) const {
  list_->AddRect(min, max, Color(color), Px(rounding_px), 0, std::max(1.0f, Px(1.0f)));
}

void Painter::Shadow(ImVec2 min, ImVec2 max, float rounding_px) const {
  // Layers spreading out and down, fainter each time.
  constexpr int kLayers = 6;
  for (int i = kLayers; i >= 1; --i) {
    float spread = Px(2.5f * float(i));
    float drop = Px(1.5f * float(i));
    float layer_alpha = 0.22f * (1.0f - float(i - 1) / float(kLayers));
    list_->AddRectFilled(ImVec2(min.x - spread, min.y - spread + drop), ImVec2(max.x + spread, max.y + spread + drop),
                         Color(style_.shadow, layer_alpha), Px(rounding_px) + spread);
  }
}

std::string FormatDate(uint64_t filetime, uint32_t language) {
  constexpr uint64_t kUnixEpoch = 116'444'736'000'000'000ULL;  // 1970-01-01 in FILETIME units (100 ns)
  if (filetime < kUnixEpoch) {
    return {};
  }
  std::time_t seconds = std::time_t((filetime - kUnixEpoch) / 10'000'000ULL);
  std::tm local = {};
#if REX_PLATFORM_WIN32
  if (localtime_s(&local, &seconds) != 0) {
    return {};
  }
#else
  if (!localtime_r(&seconds, &local)) {
    return {};
  }
#endif
  int day = local.tm_mday, month = local.tm_mon + 1, year = local.tm_year + 1900;
  char text[32];
  switch (language) {
    case 1:  // English
      std::snprintf(text, sizeof(text), "%02d/%02d/%04d", month, day, year);
      break;
    case 2:   // Japanese
    case 7:   // Korean
    case 8:   // Traditional Chinese
    case 10:  // Simplified Chinese
      std::snprintf(text, sizeof(text), "%04d/%02d/%02d", year, month, day);
      break;
    case 3:   // German
    case 11:  // Polish
    case 12:  // Russian
      std::snprintf(text, sizeof(text), "%02d.%02d.%04d", day, month, year);
      break;
    default:
      std::snprintf(text, sizeof(text), "%02d/%02d/%04d", day, month, year);
      break;
  }
  return text;
}

}  // namespace rex::ui::dialog
