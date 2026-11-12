/**
 * ReXGlue online play: the prompt for an invite received from a friend's game (invite_prompt.h).
 */

#include <rex/ui/overlay/invite_prompt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/overlay_language.h>
#include <rex/ui/windowed_app_context.h>

#include "ui/overlay/dialog_draw.h"

namespace rex::ui {
namespace {

constexpr uint32_t kMaxUsers = 4;
// How often a waiting invite asks whether it may be shown (the game's condition).
constexpr auto kWaitPoll = std::chrono::milliseconds(250);

struct Texts {
  const char* title;
  const char* invites;  // "{name} invites you to play." with {} for the name
  const char* join;
  const char* ignore;
};

// XLanguage (OverlayLanguage()) -> texts; the others get English.
const Texts& TextsFor(uint32_t language) {
  static const Texts kEnglish = {"Invite", "{} invites you to play.", "Join", "Ignore"};
  static const Texts kGerman = {"Einladung", "{} lädt dich zum Spielen ein.", "Beitreten", "Ignorieren"};
  static const Texts kFrench = {"Invitation", "{} vous invite à jouer.", "Rejoindre", "Ignorer"};
  static const Texts kSpanish = {"Invitación", "{} te invita a jugar.", "Unirse", "Ignorar"};
  static const Texts kItalian = {"Invito", "{} ti invita a giocare.", "Unisciti", "Ignora"};
  static const Texts kPortuguese = {"Convite", "{} convida você para jogar.", "Entrar", "Ignorar"};
  switch (language) {
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

class InvitePromptDialog;

struct State {
  std::mutex mutex;
  ImGuiDrawer* imgui_drawer = nullptr;
  WindowedAppContext* app_context = nullptr;
  QuitPromptInput input;
  std::function<bool()> condition;
  // The invite waiting or shown: its inviter and answer; generation tells a newer one apart.
  std::string inviter;
  std::function<void(bool)> answer;
  uint64_t generation = 0;
  bool waiting = false;  // a watcher is waiting for the condition
  bool open = false;     // the dialog is shown; the input blocker is held
  InvitePromptDialog* dialog = nullptr;  // UI thread only
};

State& GetState() {
  static State state;
  return state;
}

void Answer(bool accept);

class InvitePromptDialog final : public ImGuiDialog {
 public:
  explicit InvitePromptDialog(ImGuiDrawer* imgui_drawer) : ImGuiDialog(imgui_drawer) {}

  void Dismiss() { Close(); }

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
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      Answer(true);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      Answer(false);
    }

    const Texts& texts = TextsFor(OverlayLanguage());
    std::string inviter;
    {
      State& state = GetState();
      std::lock_guard lock(state.mutex);
      inviter = state.inviter;
    }
    std::string message = texts.invites;
    size_t at = message.find("{}");
    if (at != std::string::npos) {
      message.replace(at, 2, inviter);
    }

    using dialog::Font;
    float scale = dialog::Painter::ScaleFor(io);
    dialog::Painter back(imgui_drawer(), ImGui::GetBackgroundDrawList(), scale);
    const DialogStyle& s = back.style();
    constexpr float kPad = 28.0f, kTitleEm = 13.0f, kMessageEm = 18.0f, kChoiceHeight = 40.0f, kGap = 12.0f;
    float max_text = std::min(back.Px(520.0f), io.DisplaySize.x - back.Px(96.0f));
    float message_width = std::min(back.Measure(Font::kSemiBold, kMessageEm, message), max_text);
    float message_height =
        back.Paragraph(Font::kSemiBold, kMessageEm, ImVec2(0, 0), max_text, s.text, message, 0, false);
    float join_width = ChoiceWidth(back, texts.join);
    float ignore_width = ChoiceWidth(back, texts.ignore);
    float choices_width = join_width + back.Px(kGap) + ignore_width;
    float width = std::max({message_width, choices_width, back.Px(300.0f)}) + back.Px(kPad) * 2.0f;
    float title_height = back.LineHeight(kTitleEm);
    float height = back.Px(kPad) * 2.0f + title_height + back.Px(8.0f) + message_height + back.Px(22.0f) +
                   back.Px(kChoiceHeight);
    ImVec2 min(std::floor((io.DisplaySize.x - width) * 0.5f), std::floor((io.DisplaySize.y - height) * 0.5f));
    ImVec2 max(min.x + width, min.y + height);

    back.list()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, back.Color(s.dim_background));
    back.Shadow(min, max, s.rounding);
    back.Rect(min, max, s.background, s.rounding);
    back.Border(min, max, s.border, s.rounding);

    ImGui::SetNextWindowPos(min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##invite_prompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    dialog::Painter p(imgui_drawer(), ImGui::GetWindowDrawList(), scale);
    float title_width = p.Measure(Font::kCondensedBold, kTitleEm, texts.title, 0.08f);
    p.Text(Font::kCondensedBold, kTitleEm, ImVec2(min.x + (width - title_width) * 0.5f, min.y + p.Px(kPad)),
           s.accent, texts.title, 0.08f);
    p.Paragraph(Font::kSemiBold, kMessageEm,
                ImVec2(min.x + (width - max_text) * 0.5f, min.y + p.Px(kPad) + title_height + p.Px(8.0f)),
                max_text, s.text, message, 0, true, 0.5f);
    float choice_y = max.y - p.Px(kPad) - p.Px(kChoiceHeight);
    float choice_x = min.x + (width - choices_width) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(choice_x, choice_y));
    if (Choice(p, "##join", 'A', ImVec4(0.376f, 0.690f, 0.188f, 1.0f), texts.join, join_width)) {
      Answer(true);
    }
    ImGui::SetCursorScreenPos(ImVec2(choice_x + join_width + p.Px(kGap), choice_y));
    if (Choice(p, "##ignore", 'B', ImVec4(0.784f, 0.188f, 0.188f, 1.0f), texts.ignore, ignore_width)) {
      Answer(false);
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }

 private:
  // A new press of A joins, of B ignores; the first read only records what is held.
  void PollController() {
    State& state = GetState();
    if (!state.input.read) {
      return;
    }
    bool accept = false;
    bool decline = false;
    for (uint32_t user = 0; user < kMaxUsers; ++user) {
      rex::input::X_INPUT_GAMEPAD gamepad = {};
      uint16_t buttons = state.input.read(user, gamepad) ? static_cast<uint16_t>(gamepad.buttons) : 0;
      uint16_t pressed = primed_ ? buttons & ~previous_[user] : 0;
      previous_[user] = buttons;
      accept = accept || (pressed & rex::input::X_INPUT_GAMEPAD_A) != 0;
      decline = decline || (pressed & rex::input::X_INPUT_GAMEPAD_B) != 0;
    }
    primed_ = true;
    if (accept) {
      Answer(true);
    } else if (decline) {
      Answer(false);
    }
  }

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

// UI thread: the dialog for the invite waiting, with the input blocked.
void OpenDialog(uint64_t generation) {
  State& state = GetState();
  state.app_context->CallInUIThread([generation] {
    State& state = GetState();
    std::function<void(bool)> block;
    {
      std::lock_guard lock(state.mutex);
      if (state.generation != generation || !state.answer || state.open) {
        return;
      }
      state.open = true;
      block = state.input.block;
      if (!state.dialog) {
        state.dialog = new InvitePromptDialog(state.imgui_drawer);
      }
      REXLOG_INFO("[invite_prompt] {} invites you to play (A: join, B: ignore)", state.inviter);
    }
    if (block) {
      block(true);
    }
  });
}

// Any thread: waits (off the UI thread) until the game allows the dialog, then opens it, unless a newer invite
// came.
void WaitAndOpen(uint64_t generation) {
  std::thread([generation] {
    State& state = GetState();
    for (;;) {
      std::function<bool()> condition;
      {
        std::lock_guard lock(state.mutex);
        if (state.generation != generation || !state.answer) {
          return;
        }
        condition = state.condition;
      }
      if (!condition || condition()) {
        break;
      }
      std::this_thread::sleep_for(kWaitPoll);
    }
    OpenDialog(generation);
  }).detach();
}

// The player chose: the dialog closes, the input is released (the buttons held stay masked until let go), and the
// answer goes to the session service.
void Answer(bool accept) {
  State& state = GetState();
  std::function<void(bool)> answer;
  std::function<void(bool)> block;
  {
    std::lock_guard lock(state.mutex);
    if (!state.open || !state.answer) {
      return;
    }
    state.open = false;
    answer = std::move(state.answer);
    state.answer = nullptr;
    block = state.input.block;
  }
  if (block) {
    block(false);
  }
  state.app_context->CallInUIThread([] {
    State& state = GetState();
    if (state.dialog) {
      state.dialog->Dismiss();
    }
  });
  REXLOG_INFO("[invite_prompt] {}", accept ? "Joining" : "Ignored");
  answer(accept);
}

}  // namespace

void SetupInvitePrompt(ImGuiDrawer* imgui_drawer, WindowedAppContext* app_context, QuitPromptInput input) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.imgui_drawer = imgui_drawer;
  state.app_context = app_context;
  state.input = std::move(input);
}

void ShowInvitePrompt(const std::string& inviter, std::function<void(bool accept)> answer) {
  State& state = GetState();
  std::function<void(bool)> replaced;
  uint64_t generation;
  bool open;
  {
    std::lock_guard lock(state.mutex);
    if (!state.imgui_drawer || !state.app_context) {
      replaced = std::move(answer);
    } else {
      // A newer invite replaces the one not answered yet.
      replaced = std::move(state.answer);
      state.inviter = inviter;
      state.answer = std::move(answer);
    }
    generation = ++state.generation;
    open = state.open;
  }
  if (replaced) {
    replaced(false);
  }
  if (!open) {
    WaitAndOpen(generation);
  }
}

void SetInvitePromptCondition(std::function<bool()> condition) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.condition = std::move(condition);
}

bool IsInvitePromptOpen() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  return state.open;
}

}  // namespace rex::ui
