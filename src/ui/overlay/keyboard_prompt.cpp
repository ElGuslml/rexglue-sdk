/**
 * @file        ui/overlay/keyboard_prompt.cpp
 * @brief       An on-screen keyboard for the text a game asks the console for (see rex/ui/overlay/keyboard_prompt.h)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <rex/ui/overlay/keyboard_prompt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/string.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/overlay_language.h>

#include "ui/overlay/dialog_draw.h"

namespace rex::ui {
namespace {

using Clock = std::chrono::steady_clock;
using dialog::Font;
using dialog::Painter;

constexpr uint32_t kMaxUsers = 4;
constexpr int kColumns = 10;
constexpr int kCharRows = 4;
constexpr int kRows = kCharRows + 1;  // The last row: the actions.

// The layout, in pixels of a 720-line screen.
constexpr float kPad = 24.0f, kKeyWidth = 50.0f, kKeyHeight = 42.0f, kKeyGap = 6.0f, kKeyRounding = 8.0f;
constexpr float kFieldHeight = 46.0f, kLegendHeight = 22.0f, kLegendGap = 6.0f;
constexpr float kTitleEm = 18.0f, kDescriptionEm = 13.5f, kTextEm = 19.0f, kKeyEm = 18.0f, kWordEm = 13.0f;
constexpr float kLegendEm = 12.5f;

// A held direction, X, LB or RB repeats: first after kRepeatDelay, then every kRepeatInterval.
constexpr auto kRepeatDelay = std::chrono::milliseconds(380);
constexpr auto kRepeatInterval = std::chrono::milliseconds(80);
// What the keyboard sends in the first moments is the end of the press that opened the dialog.
constexpr auto kKeyboardGrace = std::chrono::milliseconds(200);
constexpr int16_t kStickPushed = 20000;
constexpr uint8_t kTriggerPulled = 100;

// The key sets: [symbols][shift][row], ten characters a row.
constexpr const char* kSets[2][2][kCharRows] = {
    {{"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"},
     {"1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM!?_"}},
    {{"!?@#$%&*()", "áéíóúüñçß¿", "àèìòùâêîôû", "äëïö+=/:;¡"},
     {"!?@#$%&*()", "ÁÉÍÓÚÜÑÇß¿", "ÀÈÌÒÙÂÊÎÔÛ", "ÄËÏÖ+=/:;¡"}},
};

// The last row's keys, and how many columns each takes (ten in all).
enum Action { kShift, kSet, kSpace, kCaretLeft, kCaretRight, kErase, kDone, kActionCount };
constexpr float kActionColumns[kActionCount] = {1.5f, 1.5f, 2.5f, 1.0f, 1.0f, 1.0f, 1.5f};

struct Texts {
  const char* select;
  const char* erase;
  const char* space;
  const char* shift;
  const char* symbols;
  const char* caret;
  const char* done;
  const char* cancel;
};

// XLanguage (OverlayLanguage()) -> texts; the others (Korean, Chinese, Polish, Russian) get English.
const Texts& TextsFor(uint32_t language) {
  static const Texts kEnglish = {"Select", "Erase", "Space", "Shift", "Symbols", "Cursor", "Done", "Cancel"};
  static const Texts kJapanese = {"選択", "削除", "スペース", "シフト", "記号", "カーソル", "決定", "キャンセル"};
  static const Texts kGerman = {"Auswählen", "Löschen",  "Leerzeichen", "Umschalt",
                                "Symbole",   "Cursor",   "Fertig",      "Abbrechen"};
  static const Texts kFrench = {"Sélectionner", "Effacer", "Espace", "Maj", "Symboles", "Curseur", "Terminer",
                                "Annuler"};
  static const Texts kSpanish = {"Seleccionar", "Borrar", "Espacio", "Mayús", "Símbolos", "Cursor", "Aceptar",
                                 "Cancelar"};
  static const Texts kItalian = {"Seleziona", "Cancella", "Spazio", "Maiusc", "Simboli", "Cursore", "Fine",
                                 "Annulla"};
  static const Texts kPortuguese = {"Selecionar", "Apagar", "Espaço", "Maiúsc", "Símbolos", "Cursor", "Concluir",
                                    "Cancelar"};
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

using KeySets = std::array<std::array<std::array<std::u16string, kCharRows>, 2>, 2>;

const KeySets& Sets() {
  static const KeySets sets = [] {
    KeySets result;
    for (int symbols = 0; symbols < 2; ++symbols) {
      for (int shift = 0; shift < 2; ++shift) {
        for (int row = 0; row < kCharRows; ++row) {
          std::u16string keys = rex::string::to_utf16(kSets[symbols][shift][row]);
          keys.resize(kColumns, u' ');
          result[symbols][shift][row] = std::move(keys);
        }
      }
    }
    return result;
  }();
  return sets;
}

// The controller's buttons the keyboard acts on, one bit each.
enum Pad : uint32_t {
  kPadUp = 1u << 0,
  kPadDown = 1u << 1,
  kPadLeft = 1u << 2,
  kPadRight = 1u << 3,
  kPadA = 1u << 4,
  kPadB = 1u << 5,
  kPadX = 1u << 6,
  kPadY = 1u << 7,
  kPadLB = 1u << 8,
  kPadRB = 1u << 9,
  kPadLT = 1u << 10,
  kPadRT = 1u << 11,
  kPadStart = 1u << 12,
  kPadBits = 13,
};
constexpr uint32_t kPadRepeats = kPadUp | kPadDown | kPadLeft | kPadRight | kPadX | kPadLB | kPadRB;

// A hint of the legend: the controller's buttons (a colored disc with its letter, or a pill with its name), then
// what they do.
struct Hint {
  std::vector<const char*> buttons;
  bool disc;
  ImVec4 color;
  const char* label;
};

}  // namespace

class KeyboardPrompt::Impl {
 public:
  Impl(ImGuiDrawer* imgui_drawer, KeyboardPromptParams params)
      : drawer_(imgui_drawer), params_(std::move(params)), opened_(Clock::now()) {
    text_ = rex::string::to_utf16(params_.text);
    if (params_.max_units && text_.size() > params_.max_units) {
      text_.resize(params_.max_units);
    }
    caret_ = text_.size();
    // The cursor starts on the first letter.
    row_ = 1;
    index_ = 0;
    nav_x_ = 0.5f;
    edited_ = opened_;
  }

  std::string text() const { return rex::string::to_utf8(text_); }

  Result Draw(ImGuiIO& io) {
    now_ = Clock::now();
    PollController();
    PollKeyboard(io);
    Paint(io);
    return result_;
  }

 private:
  // --- The text.

  void Insert(char16_t c) {
    if (params_.max_units && text_.size() >= params_.max_units) {
      return;
    }
    text_.insert(caret_, 1, c);
    ++caret_;
    edited_ = now_;
  }

  void Erase() {
    if (caret_ > 0) {
      text_.erase(caret_ - 1, 1);
      --caret_;
    }
    edited_ = now_;
  }

  void EraseAfter() {
    if (caret_ < text_.size()) {
      text_.erase(caret_, 1);
    }
    edited_ = now_;
  }

  void MoveCaret(int by) {
    int64_t to = int64_t(caret_) + by;
    caret_ = size_t(std::clamp<int64_t>(to, 0, int64_t(text_.size())));
    edited_ = now_;
  }

  // --- The keys.

  static int KeysIn(int row) { return row < kCharRows ? kColumns : int(kActionCount); }

  // A key's left edge and width, in columns.
  static void Span(int row, int index, float& x, float& width) {
    if (row < kCharRows) {
      x = float(index);
      width = 1.0f;
      return;
    }
    x = 0.0f;
    for (int i = 0; i < index; ++i) {
      x += kActionColumns[i];
    }
    width = kActionColumns[index];
  }

  void Move(int dx, int dy) {
    if (dx) {
      int count = KeysIn(row_);
      index_ = (index_ + dx + count) % count;
      float x, width;
      Span(row_, index_, x, width);
      nav_x_ = x + width * 0.5f;
    }
    if (dy) {
      row_ = (row_ + dy + kRows) % kRows;
      // The key of the new row over or under where the cursor was.
      index_ = KeysIn(row_) - 1;
      for (int i = 0; i < KeysIn(row_); ++i) {
        float x, width;
        Span(row_, i, x, width);
        if (nav_x_ < x + width) {
          index_ = i;
          break;
        }
      }
    }
  }

  void Press(int row, int index) {
    if (row < kCharRows) {
      Insert(Sets()[symbols_][shift_][row][index]);
      return;
    }
    switch (index) {
      case kShift:
        shift_ = !shift_;
        break;
      case kSet:
        symbols_ = !symbols_;
        break;
      case kSpace:
        Insert(u' ');
        break;
      case kCaretLeft:
        MoveCaret(-1);
        break;
      case kCaretRight:
        MoveCaret(1);
        break;
      case kErase:
        Erase();
        break;
      case kDone:
        result_ = Result::kAccepted;
        break;
    }
  }

  // --- The input.

  void PollController() {
    if (!params_.read) {
      return;
    }
    uint32_t held = 0;
    bool any = false;
    for (uint32_t user = 0; user < kMaxUsers; ++user) {
      rex::input::X_INPUT_GAMEPAD gamepad = {};
      if (!params_.read(user, gamepad)) {
        continue;
      }
      any = true;
      const uint16_t buttons = gamepad.buttons;
      int x = int16_t(gamepad.thumb_lx), y = int16_t(gamepad.thumb_ly);
      // The stick: its main direction only.
      bool horizontal = std::abs(x) > std::abs(y);
      if ((buttons & rex::input::X_INPUT_GAMEPAD_DPAD_UP) || (!horizontal && y > kStickPushed)) held |= kPadUp;
      if ((buttons & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) || (!horizontal && y < -kStickPushed)) held |= kPadDown;
      if ((buttons & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) || (horizontal && x < -kStickPushed)) held |= kPadLeft;
      if ((buttons & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) || (horizontal && x > kStickPushed)) held |= kPadRight;
      if (buttons & rex::input::X_INPUT_GAMEPAD_A) held |= kPadA;
      if (buttons & rex::input::X_INPUT_GAMEPAD_B) held |= kPadB;
      if (buttons & rex::input::X_INPUT_GAMEPAD_X) held |= kPadX;
      if (buttons & rex::input::X_INPUT_GAMEPAD_Y) held |= kPadY;
      if (buttons & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) held |= kPadLB;
      if (buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) held |= kPadRB;
      if (gamepad.left_trigger > kTriggerPulled) held |= kPadLT;
      if (gamepad.right_trigger > kTriggerPulled) held |= kPadRT;
      if (buttons & rex::input::X_INPUT_GAMEPAD_START) held |= kPadStart;
    }
    has_controller_ = any;
    if (!primed_) {
      // The first read only records what is held: the press that opened the keyboard.
      primed_ = true;
      previous_ = held;
      return;
    }
    uint32_t fired = 0;
    for (uint32_t i = 0; i < kPadBits; ++i) {
      uint32_t bit = 1u << i;
      if (!(held & bit)) {
        continue;
      }
      if (!(previous_ & bit)) {
        fired |= bit;
        repeat_at_[i] = now_ + kRepeatDelay;
      } else if ((kPadRepeats & bit) && now_ >= repeat_at_[i]) {
        fired |= bit;
        repeat_at_[i] = now_ + kRepeatInterval;
      }
    }
    previous_ = held;

    if (fired & kPadUp) Move(0, -1);
    if (fired & kPadDown) Move(0, 1);
    if (fired & kPadLeft) Move(-1, 0);
    if (fired & kPadRight) Move(1, 0);
    if (fired & kPadA) Press(row_, index_);
    if (fired & kPadX) Erase();
    if (fired & kPadY) Insert(u' ');
    if (fired & kPadLB) MoveCaret(-1);
    if (fired & kPadRB) MoveCaret(1);
    if (fired & kPadLT) shift_ = !shift_;
    if (fired & kPadRT) symbols_ = !symbols_;
    if (fired & kPadStart) {
      result_ = Result::kAccepted;
    } else if (fired & kPadB) {
      result_ = Result::kCancelled;
    }
  }

  void PollKeyboard(ImGuiIO& io) {
    if (now_ - opened_ < kKeyboardGrace) {
      return;
    }
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
      unsigned int c = io.InputQueueCharacters[i];
      if (c >= 0x20 && c != 0x7F && c < 0x10000 && !(c >= 0xD800 && c <= 0xDFFF)) {
        Insert(char16_t(c));
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) Erase();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) EraseAfter();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) MoveCaret(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) MoveCaret(1);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) MoveCaret(-int(text_.size()));
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) MoveCaret(int(text_.size()));
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      result_ = Result::kAccepted;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      result_ = Result::kCancelled;
    }
  }

  // --- The drawing.

  std::vector<Hint> Hints(const Texts& texts) const {
    static const ImVec4 kGreen(0.376f, 0.690f, 0.188f, 1.0f), kRed(0.784f, 0.188f, 0.188f, 1.0f);
    static const ImVec4 kBlue(0.200f, 0.459f, 0.820f, 1.0f), kYellow(0.820f, 0.627f, 0.110f, 1.0f);
    static const ImVec4 kNone(0, 0, 0, 0);
    if (!has_controller_) {
      return {{{"Enter"}, false, kNone, texts.done}, {{"Esc"}, false, kNone, texts.cancel}};
    }
    return {{{"A"}, true, kGreen, texts.select},      {{"X"}, true, kBlue, texts.erase},
            {{"Y"}, true, kYellow, texts.space},      {{"LB", "RB"}, false, kNone, texts.caret},
            {{"LT"}, false, kNone, texts.shift},      {{"RT"}, false, kNone, texts.symbols},
            {{"START"}, false, kNone, texts.done},    {{"B"}, true, kRed, texts.cancel}};
  }

  static float HintWidth(const Painter& p, const Hint& hint) {
    float width = 0.0f;
    for (const char* button : hint.buttons) {
      width += hint.disc ? p.Px(kLegendHeight)
                         : p.Measure(Font::kCondensedBold, kLegendEm - 1.0f, button) + p.Px(14.0f);
      width += p.Px(4.0f);
    }
    return width + p.Px(3.0f) + p.Measure(Font::kRegular, kLegendEm, hint.label);
  }

  static void DrawHint(const Painter& p, const Hint& hint, ImVec2 pos) {
    const DialogStyle& s = p.style();
    float height = p.Px(kLegendHeight);
    float x = pos.x;
    for (const char* button : hint.buttons) {
      float em = hint.disc ? kLegendEm + 0.5f : kLegendEm - 1.0f;
      float text_width = p.Measure(Font::kCondensedBold, em, button);
      float width = hint.disc ? height : text_width + p.Px(14.0f);
      if (hint.disc) {
        p.list()->AddCircleFilled(ImVec2(x + height * 0.5f, pos.y + height * 0.5f), height * 0.5f,
                                  p.Color(hint.color));
      } else {
        p.Rect(ImVec2(x, pos.y), ImVec2(x + width, pos.y + height), s.hover, kLegendHeight * 0.5f);
      }
      p.Text(Font::kCondensedBold, em, ImVec2(x + (width - text_width) * 0.5f, pos.y + (height - p.LineHeight(em)) * 0.5f),
             hint.disc ? ImVec4(1, 1, 1, 1) : s.text, button);
      x += width + p.Px(4.0f);
    }
    p.Text(Font::kRegular, kLegendEm, ImVec2(x + p.Px(3.0f), pos.y + (height - p.LineHeight(kLegendEm)) * 0.5f),
           s.muted, hint.label);
  }

  // A label centered in a key, in a smaller size when it does not fit.
  static void KeyLabel(const Painter& p, Font font, float em, ImVec2 min, ImVec2 max, const ImVec4& color,
                       std::string_view label) {
    float room = max.x - min.x - p.Px(8.0f);
    float width = p.Measure(font, em, label);
    if (width > room && width > 0.0f) {
      em *= room / width;
      width = p.Measure(font, em, label);
    }
    p.Text(font, em, ImVec2(min.x + (max.x - min.x - width) * 0.5f, min.y + (max.y - min.y - p.LineHeight(em)) * 0.5f),
           color, label);
  }

  void Paint(ImGuiIO& io) {
    const Texts& texts = TextsFor(OverlayLanguage());
    // Scaled with the screen's height, and down again for a window too narrow for the card.
    float scale = Painter::ScaleFor(io);
    const float card_width_720 = kColumns * kKeyWidth + (kColumns - 1) * kKeyGap + kPad * 2.0f;
    scale = std::min(scale, io.DisplaySize.x / (card_width_720 + 16.0f));
    Painter back(drawer_, ImGui::GetBackgroundDrawList(), scale);
    const DialogStyle& s = back.style();

    const float pad = back.Px(kPad);
    const float key_step = back.Px(kKeyWidth + kKeyGap);
    const float row_step = back.Px(kKeyHeight + kKeyGap);
    const float inner = back.Px(kColumns * kKeyWidth + (kColumns - 1) * kKeyGap);
    const float width = inner + pad * 2.0f;

    // The legend's lines: as many hints as fit in each.
    std::vector<Hint> hints = Hints(texts);
    std::vector<float> hint_widths;
    std::vector<std::pair<size_t, size_t>> lines;  // [first, last)
    {
      const float gap = back.Px(14.0f);
      size_t first = 0;
      float used = 0.0f;
      for (size_t i = 0; i < hints.size(); ++i) {
        hint_widths.push_back(HintWidth(back, hints[i]));
        float needed = (i > first ? gap : 0.0f) + hint_widths[i];
        if (i > first && used + needed > inner) {
          lines.emplace_back(first, i);
          first = i;
          used = hint_widths[i];
        } else {
          used += needed;
        }
      }
      lines.emplace_back(first, hints.size());
    }

    const float title_height = params_.title.empty() ? 0.0f : back.LineHeight(kTitleEm);
    const float description_height =
        params_.description.empty()
            ? 0.0f
            : back.Paragraph(Font::kRegular, kDescriptionEm, ImVec2(0, 0), inner, s.muted, params_.description, 2,
                             false);
    float header_height = title_height + description_height;
    if (title_height > 0.0f && description_height > 0.0f) header_height += back.Px(4.0f);
    if (header_height > 0.0f) header_height += back.Px(14.0f);
    const float keys_height = kRows * back.Px(kKeyHeight) + (kRows - 1) * back.Px(kKeyGap);
    const float legend_height =
        float(lines.size()) * back.Px(kLegendHeight) + float(lines.size() - 1) * back.Px(kLegendGap);
    const float height = pad * 2.0f + header_height + back.Px(kFieldHeight) + back.Px(14.0f) + keys_height +
                         back.Px(16.0f) + legend_height;

    ImVec2 min(std::floor((io.DisplaySize.x - width) * 0.5f),
               std::floor(std::max(0.0f, (io.DisplaySize.y - height) * 0.5f)));
    ImVec2 max(min.x + width, min.y + height);

    // The game dimmed behind the keyboard, the card.
    back.list()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, back.Color(s.dim_background));
    back.Shadow(min, max, s.rounding);
    back.Rect(min, max, s.background, s.rounding);
    back.Border(min, max, s.border, s.rounding);

    ImGui::SetNextWindowPos(min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##keyboard_prompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    Painter p(drawer_, ImGui::GetWindowDrawList(), scale);

    float x0 = min.x + pad;
    float y = min.y + pad;
    if (title_height > 0.0f) {
      p.Text(Font::kSemiBold, kTitleEm, ImVec2(x0, y), s.text, p.Ellipsize(Font::kSemiBold, kTitleEm, params_.title, inner));
      y += title_height + (description_height > 0.0f ? p.Px(4.0f) : 0.0f);
    }
    if (description_height > 0.0f) {
      p.Paragraph(Font::kRegular, kDescriptionEm, ImVec2(x0, y), inner, s.muted, params_.description, 2);
      y += description_height;
    }
    if (header_height > 0.0f) {
      y += p.Px(14.0f);
    }

    PaintField(p, ImVec2(x0, y), inner);
    y += p.Px(kFieldHeight) + p.Px(14.0f);

    // The keys.
    const KeySets& sets = Sets();
    for (int row = 0; row < kRows; ++row) {
      for (int index = 0; index < KeysIn(row); ++index) {
        float column, columns;
        Span(row, index, column, columns);
        ImVec2 key_min(x0 + std::round(column * key_step), y + float(row) * row_step);
        ImVec2 key_max(x0 + std::round((column + columns) * key_step) - p.Px(kKeyGap), key_min.y + p.Px(kKeyHeight));
        ImGui::SetCursorScreenPos(key_min);
        ImGui::PushID(row * 16 + index);
        bool clicked = ImGui::InvisibleButton("##key", ImVec2(key_max.x - key_min.x, key_max.y - key_min.y));
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (clicked) {
          row_ = row;
          index_ = index;
          nav_x_ = column + columns * 0.5f;
          Press(row, index);
        }
        bool selected = row == row_ && index == index_;
        bool lit = row == kCharRows && ((index == kShift && shift_) || (index == kSet && symbols_));
        p.Rect(key_min, key_max, selected ? s.accent : hovered ? s.hover : s.raised, kKeyRounding);
        if (lit && !selected) {
          p.Border(key_min, key_max, s.accent, kKeyRounding);
        }
        const ImVec4& ink = selected ? s.on_accent : (lit || (row == kCharRows && index == kDone)) ? s.accent : s.text;
        if (row < kCharRows) {
          KeyLabel(p, Font::kSemiBold, kKeyEm, key_min, key_max, ink,
                   rex::string::to_utf8(std::u16string_view(&sets[symbols_][shift_][row][index], 1)));
          continue;
        }
        ImVec2 center((key_min.x + key_max.x) * 0.5f, (key_min.y + key_max.y) * 0.5f);
        ImU32 ink_color = p.Color(ink);
        float u = p.Px(1.0f);
        switch (index) {
          case kShift:
            KeyLabel(p, Font::kSemiBold, kWordEm, key_min, key_max, ink, texts.shift);
            break;
          case kSet:
            KeyLabel(p, Font::kSemiBold, kWordEm + 1.0f, key_min, key_max, ink, symbols_ ? "abc" : "#+=");
            break;
          case kSpace:
            KeyLabel(p, Font::kSemiBold, kWordEm, key_min, key_max, ink, texts.space);
            break;
          case kCaretLeft:
            p.list()->AddTriangleFilled(ImVec2(center.x - 6 * u, center.y), ImVec2(center.x + 4 * u, center.y - 7 * u),
                                        ImVec2(center.x + 4 * u, center.y + 7 * u), ink_color);
            break;
          case kCaretRight:
            p.list()->AddTriangleFilled(ImVec2(center.x + 6 * u, center.y), ImVec2(center.x - 4 * u, center.y - 7 * u),
                                        ImVec2(center.x - 4 * u, center.y + 7 * u), ink_color);
            break;
          case kErase: {
            // The erase key's sign: a box pointing left with a cross.
            const ImVec2 outline[] = {ImVec2(center.x - 12 * u, center.y),        ImVec2(center.x - 5 * u, center.y - 8 * u),
                                      ImVec2(center.x + 11 * u, center.y - 8 * u), ImVec2(center.x + 11 * u, center.y + 8 * u),
                                      ImVec2(center.x - 5 * u, center.y + 8 * u)};
            p.list()->AddPolyline(outline, 5, ink_color, ImDrawFlags_Closed, 1.6f * u);
            p.list()->AddLine(ImVec2(center.x - 1 * u, center.y - 4 * u), ImVec2(center.x + 7 * u, center.y + 4 * u),
                              ink_color, 1.6f * u);
            p.list()->AddLine(ImVec2(center.x + 7 * u, center.y - 4 * u), ImVec2(center.x - 1 * u, center.y + 4 * u),
                              ink_color, 1.6f * u);
            break;
          }
          case kDone:
            KeyLabel(p, Font::kSemiBold, kWordEm, key_min, key_max, ink, texts.done);
            break;
        }
      }
    }
    y += keys_height + p.Px(16.0f);

    // The legend, each line centered.
    for (const auto& [first, last] : lines) {
      const float gap = p.Px(14.0f);
      float line_width = 0.0f;
      for (size_t i = first; i < last; ++i) {
        line_width += hint_widths[i] + (i > first ? gap : 0.0f);
      }
      float x = x0 + std::max(0.0f, (inner - line_width) * 0.5f);
      for (size_t i = first; i < last; ++i) {
        DrawHint(p, hints[i], ImVec2(x, y));
        x += hint_widths[i] + gap;
      }
      y += p.Px(kLegendHeight + kLegendGap);
    }

    ImGui::End();
    ImGui::PopStyleVar();
  }

  // The text with its caret, and how much of the game's buffer it fills.
  void PaintField(const Painter& p, ImVec2 pos, float width) {
    const DialogStyle& s = p.style();
    ImVec2 max(pos.x + width, pos.y + p.Px(kFieldHeight));
    p.Rect(pos, max, s.raised, kKeyRounding);
    p.Border(pos, max, s.accent, kKeyRounding);

    float right = max.x - p.Px(14.0f);
    if (params_.max_units) {
      char counter[32];
      std::snprintf(counter, sizeof(counter), "%zu / %zu", text_.size(), params_.max_units);
      float counter_width = p.Measure(Font::kRegular, 12.0f, counter);
      p.Text(Font::kRegular, 12.0f, ImVec2(right - counter_width, pos.y + (p.Px(kFieldHeight) - p.LineHeight(12.0f)) * 0.5f),
             text_.size() >= params_.max_units ? s.accent : s.dim, counter);
      right -= counter_width + p.Px(12.0f);
    }
    float left = pos.x + p.Px(14.0f);
    float room = std::max(1.0f, right - left);
    std::string text = rex::string::to_utf8(text_);
    std::string before = rex::string::to_utf8(std::u16string_view(text_).substr(0, caret_));
    float caret_x = p.Measure(Font::kRegular, kTextEm, before);
    float text_width = p.Measure(Font::kRegular, kTextEm, text);
    // The text scrolls so that the caret stays in the field.
    scroll_ = std::min(scroll_, caret_x);
    scroll_ = std::max(scroll_, caret_x - room + p.Px(2.0f));
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, text_width - room + p.Px(2.0f)));
    float line = p.LineHeight(kTextEm);
    float top = pos.y + (p.Px(kFieldHeight) - line) * 0.5f;
    p.list()->PushClipRect(ImVec2(left, pos.y), ImVec2(right, max.y), true);
    p.Text(Font::kRegular, kTextEm, ImVec2(left - scroll_, top), s.text, text);
    // The caret: steady for half a second after an edit, then blinking.
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now_ - edited_).count();
    if ((since / 530) % 2 == 0) {
      float x = std::floor(left - scroll_ + caret_x);
      p.list()->AddRectFilled(ImVec2(x, top + line * 0.08f), ImVec2(x + std::max(1.0f, p.Px(2.0f)), top + line * 0.92f),
                              p.Color(s.accent));
    }
    p.list()->PopClipRect();

    // The keyboard's characters are asked for as ImGui's own text boxes ask: the window sends them only then (and
    // an input method opens at the caret).
    ImGuiPlatformImeData& ime = ImGui::GetCurrentContext()->PlatformImeData;
    ime.WantVisible = true;
    ime.WantTextInput = true;
    ime.InputPos = ImVec2(left - scroll_ + caret_x, top);
    ime.InputLineHeight = line;
    ime.ViewportId = ImGui::GetMainViewport()->ID;
  }

  ImGuiDrawer* drawer_;
  KeyboardPromptParams params_;
  Clock::time_point opened_;
  Clock::time_point now_{};
  Clock::time_point edited_{};
  Result result_ = Result::kOpen;

  std::u16string text_;
  size_t caret_ = 0;
  float scroll_ = 0.0f;

  // The controller's cursor: a row, the key in it, and its center in columns (kept when moving up and down).
  int row_ = 0;
  int index_ = 0;
  float nav_x_ = 0.0f;
  bool shift_ = false;
  bool symbols_ = false;

  bool has_controller_ = false;
  bool primed_ = false;
  uint32_t previous_ = 0;
  std::array<Clock::time_point, kPadBits> repeat_at_{};
};

KeyboardPrompt::KeyboardPrompt(ImGuiDrawer* imgui_drawer, KeyboardPromptParams params)
    : impl_(std::make_unique<Impl>(imgui_drawer, std::move(params))) {}

KeyboardPrompt::~KeyboardPrompt() = default;

KeyboardPrompt::Result KeyboardPrompt::Draw(ImGuiIO& io) {
  return impl_->Draw(io);
}

std::string KeyboardPrompt::text() const {
  return impl_->text();
}

}  // namespace rex::ui
