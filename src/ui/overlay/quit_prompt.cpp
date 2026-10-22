/**
 * @file        ui/overlay/quit_prompt.cpp
 * @brief       A confirmation before quitting (see rex/ui/overlay/quit_prompt.h)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <rex/ui/overlay/quit_prompt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/flags.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/overlay_language.h>
#include <rex/ui/windowed_app_context.h>

#include "ui/overlay/dialog_draw.h"

REXCVAR_DEFINE_BOOL(quit_prompt, true, "UI",
                    "Ask before quitting when the game offers it: B on a screen such as its title screen opens "
                    "a confirmation (A quits, B cancels)");

namespace rex::ui {
namespace {

using Clock = std::chrono::steady_clock;

// The prompt counts as offered for this long after the game's last OfferQuitPrompt(): a few frames at 30 FPS,
// so that a guest read between two offers still sees it.
constexpr auto kOfferWindow = std::chrono::milliseconds(150);
constexpr uint32_t kMaxUsers = 4;

struct Texts {
  const char* question;
  const char* quit;
  const char* cancel;
};

// XLanguage (OverlayLanguage()) -> texts; the others (Korean, Chinese, Polish, Russian) get English.
const Texts& TextsFor(uint32_t language) {
  static const Texts kEnglish = {"Are you sure you want to quit the game?", "Quit", "Cancel"};
  static const Texts kJapanese = {"ゲームを終了しますか？", "終了", "キャンセル"};
  static const Texts kGerman = {"Möchtest du das Spiel wirklich beenden?", "Beenden", "Abbrechen"};
  static const Texts kFrench = {"Voulez-vous vraiment quitter le jeu ?", "Quitter", "Annuler"};
  static const Texts kSpanish = {"¿Estás seguro de que quieres salir del juego?", "Salir", "Cancelar"};
  static const Texts kItalian = {"Vuoi davvero uscire dal gioco?", "Esci", "Annulla"};
  static const Texts kPortuguese = {"Tem certeza de que deseja sair do jogo?", "Sair", "Cancelar"};
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

class QuitPromptDialog;

// Shared between the guest threads that read the controller (the filter), the game thread that offers the
// prompt, and the UI thread that draws the dialog.
struct State {
  std::mutex mutex;
  ImGuiDrawer* imgui_drawer = nullptr;
  WindowedAppContext* app_context = nullptr;
  QuitPromptInput input;
  std::function<void()> accept;
  std::atomic<int64_t> offered_at{0};  // Clock ticks of the last OfferQuitPrompt(); 0 = never
  std::function<bool()> condition;     // SetQuitPromptCondition(): asked when a press of B begins
  bool open = false;                   // the dialog is open (or opening); the input blocker is held
  bool accepted = false;               // the player confirmed: the window is closing
  std::array<uint16_t, kMaxUsers> buttons{};  // each user's buttons at the guest's last read (the filter)
  QuitPromptDialog* dialog = nullptr;         // UI thread only
};

State& GetState() {
  static State state;
  return state;
}

void Accept();
void Cancel();

class QuitPromptDialog final : public ImGuiDialog {
 public:
  explicit QuitPromptDialog(ImGuiDrawer* imgui_drawer) : ImGuiDialog(imgui_drawer) {}

  // UI thread: closes the dialog (it deletes itself on its next Draw).
  void Dismiss() { Close(); }

  // Drawn with the game's frames, not at the host's refresh rate: the game keeps running behind the dialog
  // (asking for continuous painting made the Xenos presenter repaint at 220 FPS with nothing new to show).
  bool WantsContinuousPaint() override { return false; }

 protected:
  void OnClose() override {
    State& state = GetState();
    if (state.dialog == this) {
      state.dialog = nullptr;
    }
  }

  void OnDraw(ImGuiIO& io) override {
    PollController();
    // The keyboard: Enter accepts, Escape cancels (ImGui gets the keys while a dialog is drawn).
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      Accept();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      Cancel();
    }

    const Texts& texts = TextsFor(OverlayLanguage());
    // The recomp branch's dialog style (DialogStyle, in Barlow), scaled with the screen's height.
    using dialog::Font;
    float scale = dialog::Painter::ScaleFor(io);
    dialog::Painter back(imgui_drawer(), ImGui::GetBackgroundDrawList(), scale);
    const DialogStyle& s = back.style();

    // The layout, in pixels of a 720-line screen.
    constexpr float kPad = 28.0f, kQuestionEm = 18.0f, kChoiceHeight = 40.0f, kGap = 12.0f;
    float max_text = std::min(back.Px(520.0f), io.DisplaySize.x - back.Px(96.0f));
    float question_width = std::min(back.Measure(Font::kSemiBold, kQuestionEm, texts.question), max_text);
    float question_height =
        back.Paragraph(Font::kSemiBold, kQuestionEm, ImVec2(0, 0), max_text, s.text, texts.question, 0, false);
    float quit_width = ChoiceWidth(back, texts.quit);
    float cancel_width = ChoiceWidth(back, texts.cancel);
    float choices_width = quit_width + back.Px(kGap) + cancel_width;
    float width = std::max({question_width, choices_width, back.Px(300.0f)}) + back.Px(kPad) * 2.0f;
    float height = back.Px(kPad) * 2.0f + question_height + back.Px(22.0f) + back.Px(kChoiceHeight);
    ImVec2 min(std::floor((io.DisplaySize.x - width) * 0.5f), std::floor((io.DisplaySize.y - height) * 0.5f));
    ImVec2 max(min.x + width, min.y + height);

    // The game dimmed behind the dialog, the card.
    back.list()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, back.Color(s.dim_background));
    back.Shadow(min, max, s.rounding);
    back.Rect(min, max, s.background, s.rounding);
    back.Border(min, max, s.border, s.rounding);

    ImGui::SetNextWindowPos(min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##quit_prompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    dialog::Painter p(imgui_drawer(), ImGui::GetWindowDrawList(), scale);
    // The question, centered line by line.
    p.Paragraph(Font::kSemiBold, kQuestionEm, ImVec2(min.x + (width - max_text) * 0.5f, min.y + p.Px(kPad)),
                max_text, s.text, texts.question, 0, true, 0.5f);
    // The two choices, centered under the question.
    float choice_y = max.y - p.Px(kPad) - p.Px(kChoiceHeight);
    float choice_x = min.x + (width - choices_width) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(choice_x, choice_y));
    if (Choice(p, "##quit", 'A', ImVec4(0.376f, 0.690f, 0.188f, 1.0f), texts.quit, quit_width)) {
      Accept();
    }
    ImGui::SetCursorScreenPos(ImVec2(choice_x + quit_width + p.Px(kGap), choice_y));
    if (Choice(p, "##cancel", 'B', ImVec4(0.784f, 0.188f, 0.188f, 1.0f), texts.cancel, cancel_width)) {
      Cancel();
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }

 private:
  // The controller, read for the UI while the guest is blocked: a new press of A accepts, of B cancels. The first
  // read only records what is held (the B that opened the dialog).
  void PollController() {
    State& state = GetState();
    if (!state.input.read) {
      return;
    }
    bool accept = false;
    bool cancel = false;
    for (uint32_t user = 0; user < kMaxUsers; ++user) {
      rex::input::X_INPUT_GAMEPAD gamepad = {};
      uint16_t buttons = state.input.read(user, gamepad) ? static_cast<uint16_t>(gamepad.buttons) : 0;
      uint16_t pressed = primed_ ? buttons & ~previous_[user] : 0;
      previous_[user] = buttons;
      accept = accept || (pressed & rex::input::X_INPUT_GAMEPAD_A) != 0;
      cancel = cancel || (pressed & rex::input::X_INPUT_GAMEPAD_B) != 0;
    }
    primed_ = true;
    if (accept) {
      Accept();
    } else if (cancel) {
      Cancel();
    }
  }

  // A choice: a rounded button with the controller button as a colored disc with its letter, then the label.
  // Clickable.
  static float ChoiceWidth(const dialog::Painter& p, const char* label) {
    return p.Px(14.0f + 24.0f + 10.0f + 18.0f) + p.Measure(dialog::Font::kSemiBold, 15.0f, label);
  }

  bool Choice(const dialog::Painter& p, const char* id, char letter, const ImVec4& color, const char* label,
              float width) {
    using dialog::Font;
    const DialogStyle& s = p.style();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size(width, p.Px(40.0f));
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 max(origin.x + size.x, origin.y + size.y);
    p.Rect(origin, max, hovered ? s.hover : s.raised, 20.0f);
    float disc = p.Px(24.0f);
    ImVec2 center(origin.x + p.Px(14.0f) + disc * 0.5f, origin.y + size.y * 0.5f);
    p.list()->AddCircleFilled(center, disc * 0.5f, p.Color(color));
    char text[2] = {letter, 0};
    float letter_width = p.Measure(Font::kCondensedBold, 14.0f, text);
    p.Text(Font::kCondensedBold, 14.0f,
           ImVec2(center.x - letter_width * 0.5f, center.y - p.LineHeight(14.0f) * 0.5f), ImVec4(1, 1, 1, 1), text);
    p.Text(Font::kSemiBold, 15.0f,
           ImVec2(origin.x + p.Px(14.0f) + disc + p.Px(10.0f), origin.y + (size.y - p.LineHeight(15.0f)) * 0.5f),
           s.text, label);
    return clicked;
  }

  std::array<uint16_t, kMaxUsers> previous_{};
  bool primed_ = false;
};

// The dialog is created and closed on the UI thread; these post there.
void OpenDialog() {
  State& state = GetState();
  if (!state.app_context || !state.imgui_drawer) {
    return;
  }
  state.app_context->CallInUIThread([] {
    State& state = GetState();
    std::lock_guard lock(state.mutex);
    if (state.open && !state.dialog) {
      state.dialog = new QuitPromptDialog(state.imgui_drawer);
      REXLOG_INFO("[quit_prompt] Quit the game? (A: quit, B: cancel)");
    }
  });
}

void CloseDialog(bool accepted) {
  State& state = GetState();
  if (!state.app_context) {
    return;
  }
  state.app_context->CallInUIThread([accepted] {
    State& state = GetState();
    std::function<void()> accept;
    {
      std::lock_guard lock(state.mutex);
      if (state.dialog) {
        state.dialog->Dismiss();
      }
      if (accepted) {
        accept = state.accept;
      }
    }
    REXLOG_INFO("[quit_prompt] {}", accepted ? "Quitting" : "Cancelled");
    if (accept) {
      accept();
    }
  });
}

// Any thread: the player chose. After an accept the blocker stays held until the process exits; after a cancel it
// is released, and the buttons still held stay masked from the guest until let go (RemoveUIInputBlocker).
void Accept() {
  State& state = GetState();
  {
    std::lock_guard lock(state.mutex);
    if (!state.open || state.accepted) {
      return;
    }
    state.accepted = true;
  }
  CloseDialog(true);
}

void Cancel() {
  State& state = GetState();
  std::function<void(bool)> block;
  {
    std::lock_guard lock(state.mutex);
    if (!state.open || state.accepted) {
      return;
    }
    state.open = false;
    block = state.input.block;
  }
  if (block) {
    block(false);
  }
  CloseDialog(false);
}

}  // namespace

void SetupQuitPrompt(ImGuiDrawer* imgui_drawer, WindowedAppContext* app_context, QuitPromptInput input,
                     std::function<void()> accept) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.imgui_drawer = imgui_drawer;
  state.app_context = app_context;
  state.input = std::move(input);
  state.accept = std::move(accept);
}

void OfferQuitPrompt() {
  if (!REXCVAR_GET(quit_prompt)) {
    return;
  }
  GetState().offered_at.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

void SetQuitPromptCondition(std::function<bool()> condition) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.condition = std::move(condition);
}

void SetQuitPromptLanguage(uint32_t xlanguage) {
  SetOverlayLanguage(xlanguage);
}

bool IsQuitPromptOpen() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  return state.open;
}

bool QuitPromptInputFilter(uint32_t user_index, const rex::input::X_INPUT_GAMEPAD& gamepad) {
  State& state = GetState();
  uint32_t user = std::min(user_index, kMaxUsers - 1);
  uint16_t buttons = gamepad.buttons;
  std::function<void(bool)> block;
  {
    std::lock_guard lock(state.mutex);
    uint16_t pressed = buttons & ~state.buttons[user];
    state.buttons[user] = buttons;
    if (state.open || !(pressed & rex::input::X_INPUT_GAMEPAD_B)) {
      return false;
    }
    int64_t offered_at = state.offered_at.load(std::memory_order_relaxed);
    auto since = Clock::now().time_since_epoch() - Clock::duration(offered_at);
    bool offered = (offered_at != 0 && since < kOfferWindow) || (state.condition && state.condition());
    if (!offered || !REXCVAR_GET(quit_prompt) || !state.imgui_drawer) {
      return false;
    }
    state.open = true;
    block = state.input.block;
  }
  // From now on the guest reads the pad at rest; this read is taken too (the B stays masked until released).
  if (block) {
    block(true);
  }
  OpenDialog();
  return true;
}

}  // namespace rex::ui
