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
#include <rex/ui/windowed_app_context.h>

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

// XLanguage (user_language) -> texts. The default ImGui font has Latin-1 and, on Windows, Japanese: the
// languages it cannot draw (Korean, Chinese, Polish, Russian) get English.
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

    const Texts& texts = TextsFor(REXCVAR_GET(user_language));
    // The default font is a 10-pixel bitmap font: scaled by whole steps with the screen's height, so that the
    // text is about 1/36 of it.
    float scale = std::max(1.0f, std::floor(io.DisplaySize.y / 360.0f));

    // The game dimmed behind the dialog.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 8.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
    ImGui::Begin("##quit_prompt", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::SetWindowFontScale(scale);
    ImGui::TextUnformatted(texts.question);
    ImGui::Dummy(ImVec2(0, 4.0f * scale));
    // The two choices, centered under the question.
    float width_a = ChoiceWidth(texts.quit, scale);
    float width_b = ChoiceWidth(texts.cancel, scale);
    float gap = 24.0f * scale;
    float total = width_a + gap + width_b;
    float offset = (ImGui::GetContentRegionAvail().x - total) * 0.5f;
    if (offset > 0) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    }
    if (Choice("##quit", 'A', IM_COL32(96, 176, 48, 255), texts.quit, scale)) {
      Accept();
    }
    ImGui::SameLine(0, gap);
    if (Choice("##cancel", 'B', IM_COL32(200, 48, 48, 255), texts.cancel, scale)) {
      Cancel();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
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

  // A choice: the controller button as a colored disc with its letter, then the label. Clickable.
  static float ChoiceWidth(const char* label, float scale) {
    float disc = ImGui::GetTextLineHeight() * 1.5f;
    return disc + 6.0f * scale + ImGui::CalcTextSize(label).x;
  }

  static bool Choice(const char* id, char letter, ImU32 color, const char* label, float scale) {
    float line = ImGui::GetTextLineHeight();
    float disc = line * 1.5f;
    ImVec2 size(ChoiceWidth(label, scale), disc);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 center(origin.x + disc * 0.5f, origin.y + disc * 0.5f);
    if (ImGui::IsItemHovered()) {
      draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(255, 255, 255, 30),
                          4.0f * scale);
    }
    draw->AddCircleFilled(center, disc * 0.5f, color);
    char text[2] = {letter, 0};
    ImVec2 letter_size = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(center.x - letter_size.x * 0.5f, center.y - letter_size.y * 0.5f),
                  IM_COL32(255, 255, 255, 255), text);
    draw->AddText(ImVec2(origin.x + disc + 6.0f * scale, origin.y + (disc - line) * 0.5f),
                  ImGui::GetColorU32(ImGuiCol_Text), label);
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
    if (offered_at == 0 || since >= kOfferWindow || !REXCVAR_GET(quit_prompt) || !state.imgui_drawer) {
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
