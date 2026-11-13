/**
 * ReXGlue online play: the notification for an invite received from a friend's game (invite_prompt.h).
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

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMaxUsers = 4;
// How often a waiting invite asks whether it may be shown (the game's condition).
constexpr auto kWaitPoll = std::chrono::milliseconds(250);
constexpr float kEnterSeconds = 0.4f;
constexpr float kExitSeconds = 0.45f;
// After the hold: "Joining..." this long before the card goes.
constexpr float kJoinedSeconds = 1.5f;

struct Texts {
  const char* title;
  const char* invites;  // "{name} invites you to play" with {} for the name
  const char* hold;     // the hint before the button, "Hold"
  const char* to_join;  // after it, "to join"
  const char* joining;
};

// XLanguage (OverlayLanguage()) -> texts; the others get English.
const Texts& TextsFor(uint32_t language) {
  static const Texts kEnglish = {"INVITE", "{} invites you to play", "Hold", "to join", "Joining..."};
  static const Texts kGerman = {"EINLADUNG", "{} lädt dich zum Spielen ein", "Halte", "zum Beitreten",
                                "Beitreten..."};
  static const Texts kFrench = {"INVITATION", "{} vous invite à jouer", "Maintenez", "pour rejoindre",
                                "Connexion..."};
  static const Texts kSpanish = {"INVITACIÓN", "{} te invita a jugar", "Mantén", "para unirte", "Uniéndote..."};
  static const Texts kItalian = {"INVITO", "{} ti invita a giocare", "Tieni premuto", "per unirti",
                                 "Ti stai unendo..."};
  static const Texts kPortuguese = {"CONVITE", "{} convida você para jogar", "Segure", "para entrar",
                                    "Entrando..."};
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

class InviteToastDialog;

struct State {
  std::mutex mutex;
  ImGuiDrawer* imgui_drawer = nullptr;
  WindowedAppContext* app_context = nullptr;
  std::function<bool()> condition;
  // The invite waiting or shown: its inviter and answer; generation tells a newer one apart.
  std::string inviter;
  std::function<void(bool)> answer;
  uint64_t generation = 0;
  bool shown = false;               // the card is up (its answer still to come)
  Clock::time_point shown_at{};     // when it came up
  bool accepted = false;            // held long enough: "Joining..." until joined_at + kJoinedSeconds
  Clock::time_point accepted_at{};
  std::array<Clock::time_point, kMaxUsers> held_since{};  // START held since (none: not held)
  bool enter_held = false;          // Enter, through ImGui (the UI thread)
  Clock::time_point enter_since{};
  InviteToastDialog* dialog = nullptr;  // UI thread only
};

State& GetState() {
  static State state;
  return state;
}

// The invite's answer, on the UI thread, once.
void Finish(bool accept) {
  State& state = GetState();
  std::function<void(bool)> answer;
  {
    std::lock_guard lock(state.mutex);
    if (!state.answer) {
      return;
    }
    answer = std::move(state.answer);
    state.answer = nullptr;
    if (accept) {
      state.accepted = true;
      state.accepted_at = Clock::now();
    } else {
      state.shown = false;
    }
    state.held_since = {};
  }
  REXLOG_INFO("[invite_prompt] {}", accept ? "Joining" : "Ignored");
  answer(accept);
}

float Seconds(Clock::duration d) {
  return std::chrono::duration<float>(d).count();
}

class InviteToastDialog final : public ImGuiDialog {
 public:
  explicit InviteToastDialog(ImGuiDrawer* imgui_drawer) : ImGuiDialog(imgui_drawer) {}

  bool WantsContinuousPaint() override {
    State& state = GetState();
    std::lock_guard lock(state.mutex);
    return state.shown || state.accepted;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    State& state = GetState();
    const auto now = Clock::now();
    std::string inviter;
    float age = 0.0f, hold = 0.0f, joined = -1.0f;
    {
      std::lock_guard lock(state.mutex);
      if (state.accepted) {
        joined = Seconds(now - state.accepted_at);
        if (joined >= kJoinedSeconds) {
          state.accepted = false;
          state.shown = false;
          return;
        }
      } else if (!state.shown) {
        return;
      }
      // Enter on the keyboard counts as START.
      bool enter = ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter);
      if (enter && !state.enter_held) {
        state.enter_since = now;
      }
      state.enter_held = enter;
      inviter = state.inviter;
      age = Seconds(now - state.shown_at);
      for (const auto& since : state.held_since) {
        if (since != Clock::time_point{}) {
          hold = std::max(hold, Seconds(now - since));
        }
      }
      if (state.enter_held) {
        hold = std::max(hold, Seconds(now - state.enter_since));
      }
    }
    if (joined < 0.0f) {
      if (hold >= kInviteHoldSeconds) {
        Finish(true);
        joined = 0.0f;
      } else if (age >= kInviteShowSeconds) {
        Finish(false);
        return;
      }
    }
    Draw(io, inviter, age, joined < 0.0f ? hold / kInviteHoldSeconds : 1.0f, joined);
  }

 private:
  void Draw(ImGuiIO& io, const std::string& inviter, float age, float hold, float joined) {
    const Texts& texts = TextsFor(OverlayLanguage());
    std::string message = texts.invites;
    size_t at = message.find("{}");
    if (at != std::string::npos) {
      message.replace(at, 2, inviter);
    }

    // In: slides down from above and fades in. Out (the time up, or after "Joining..."): fades rising a little.
    float shown_for = joined >= 0.0f ? kJoinedSeconds : kInviteShowSeconds;
    float time = joined >= 0.0f ? joined : age;
    float enter = joined >= 0.0f ? 1.0f : std::min(1.0f, age / kEnterSeconds);
    enter = 1.0f - (1.0f - enter) * (1.0f - enter) * (1.0f - enter);
    float alpha = joined >= 0.0f ? 1.0f : std::min(1.0f, age / (kEnterSeconds * 0.6f));
    float exit = std::clamp((time - (shown_for - kExitSeconds)) / kExitSeconds, 0.0f, 1.0f);
    alpha *= 1.0f - exit;
    float left = joined >= 0.0f
                     ? 0.0f
                     : 1.0f - std::clamp((age - kEnterSeconds) / (kInviteShowSeconds - kEnterSeconds - kExitSeconds),
                                         0.0f, 1.0f);

    dialog::Painter p(imgui_drawer(), ImGui::GetForegroundDrawList(), dialog::Painter::ScaleFor(io), alpha);
    const DialogStyle& s = p.style();
    using dialog::Font;

    // The layout, in pixels of a 720-line screen.
    constexpr float kMargin = 20.0f, kPad = 14.0f, kBar = 3.0f, kKeyEm = 12.0f;
    constexpr float kHeaderEm = 11.0f, kNameEm = 15.5f, kHintEm = 12.5f;
    float width = std::min(p.Px(420.0f), io.DisplaySize.x - p.Px(32.0f));
    float text_width = width - p.Px(kPad) * 2.0f;
    float key_height = p.Px(20.0f);
    float hint_height = std::max(p.LineHeight(kHintEm), key_height);
    float height = p.Px(kPad) * 2.0f + p.LineHeight(kHeaderEm) + p.Px(3.0f) + p.LineHeight(kNameEm) + p.Px(8.0f) +
                   hint_height + p.Px(kBar + 6.0f);
    float x0 = std::floor((io.DisplaySize.x - width) * 0.5f);
    float y0 = std::floor(p.Px(kMargin) - (1.0f - enter) * (height + p.Px(kMargin + 12.0f)) - exit * p.Px(10.0f));
    ImVec2 min(x0, y0), max(x0 + width, y0 + height);

    p.Shadow(min, max, s.rounding);
    p.Rect(min, max, s.background, s.rounding);
    p.Border(min, max, s.border, s.rounding);

    float tx = x0 + p.Px(kPad);
    float ty = y0 + p.Px(kPad);
    p.Text(Font::kCondensedBold, kHeaderEm, ImVec2(tx, ty), s.accent,
           p.Ellipsize(Font::kCondensedBold, kHeaderEm, texts.title, text_width), 0.14f);
    ty += p.LineHeight(kHeaderEm) + p.Px(3.0f);
    p.Text(Font::kSemiBold, kNameEm, ImVec2(tx, ty), s.text, p.Ellipsize(Font::kSemiBold, kNameEm, message, text_width));
    ty += p.LineHeight(kNameEm) + p.Px(8.0f);

    if (joined >= 0.0f) {
      p.Text(Font::kSemiBold, kHintEm, ImVec2(tx, ty + (hint_height - p.LineHeight(kHintEm)) * 0.5f), s.accent,
             texts.joining);
    } else {
      // "Hold [START] to join": the key's pill fills as it is held.
      float text_y = ty + (hint_height - p.LineHeight(kHintEm)) * 0.5f;
      p.Text(Font::kRegular, kHintEm, ImVec2(tx, text_y), s.muted, texts.hold);
      float key_x = tx + p.Measure(Font::kRegular, kHintEm, texts.hold) + p.Px(6.0f);
      const char* key = "START";
      float key_width = p.Measure(Font::kCondensedBold, kKeyEm, key, 0.08f) + p.Px(18.0f);
      ImVec2 key_min(key_x, ty + (hint_height - key_height) * 0.5f);
      ImVec2 key_max(key_min.x + key_width, key_min.y + key_height);
      p.Rect(key_min, key_max, s.raised, 10.0f);
      if (hold > 0.0f) {
        p.Rect(key_min, ImVec2(key_min.x + key_width * std::min(hold, 1.0f), key_max.y), s.accent, 10.0f);
      }
      p.Border(key_min, key_max, s.border, 10.0f);
      p.Text(Font::kCondensedBold, kKeyEm,
             ImVec2(key_min.x + p.Px(9.0f), key_min.y + (key_height - p.LineHeight(kKeyEm)) * 0.5f),
             hold > 0.5f ? s.on_accent : s.text, key, 0.08f);
      p.Text(Font::kRegular, kHintEm, ImVec2(key_max.x + p.Px(6.0f), text_y), s.muted, texts.to_join);
    }

    // The time left.
    ImVec2 bar_min(x0 + p.Px(kPad), y0 + height - p.Px(kPad * 0.5f + kBar));
    ImVec2 bar_max(x0 + width - p.Px(kPad), bar_min.y + p.Px(kBar));
    p.Rect(bar_min, bar_max, s.raised, kBar * 0.5f);
    if (left > 0.0f) {
      p.Rect(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * left, bar_max.y), s.accent, kBar * 0.5f);
    }
  }
};

// UI thread: the card for the invite waiting.
void Open(uint64_t generation) {
  State& state = GetState();
  state.app_context->CallInUIThread([generation] {
    State& state = GetState();
    std::lock_guard lock(state.mutex);
    if (state.generation != generation || !state.answer) {
      return;
    }
    state.shown = true;
    state.accepted = false;
    state.shown_at = Clock::now();
    state.held_since = {};
    state.enter_held = false;
    if (!state.dialog) {
      // It stays for the session (as the achievement toast), drawing only while an invite is up.
      state.dialog = new InviteToastDialog(state.imgui_drawer);
    }
    REXLOG_INFO("[invite_prompt] {} invites you to play (hold START to join)", state.inviter);
  });
}

// Any thread: waits (off the UI thread) until the game allows the card, then shows it, unless a newer invite came.
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
    Open(generation);
  }).detach();
}

}  // namespace

void SetupInvitePrompt(ImGuiDrawer* imgui_drawer, WindowedAppContext* app_context, QuitPromptInput input) {
  (void)input;
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.imgui_drawer = imgui_drawer;
  state.app_context = app_context;
}

void ShowInvitePrompt(const std::string& inviter, std::function<void(bool accept)> answer) {
  State& state = GetState();
  std::function<void(bool)> replaced;
  uint64_t generation;
  {
    std::lock_guard lock(state.mutex);
    if (!state.imgui_drawer || !state.app_context) {
      replaced = std::move(answer);
    } else {
      // A newer invite replaces the one not answered yet.
      replaced = std::move(state.answer);
      state.inviter = inviter;
      state.answer = std::move(answer);
      state.shown = false;
    }
    generation = ++state.generation;
  }
  if (replaced) {
    replaced(false);
  }
  WaitAndOpen(generation);
}

void SetInvitePromptCondition(std::function<bool()> condition) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.condition = std::move(condition);
}

bool IsInvitePromptOpen() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  return state.shown && !state.accepted;
}

bool InvitePromptInputFilter(uint32_t user_index, const rex::input::X_INPUT_GAMEPAD& gamepad) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  uint32_t user = std::min(user_index, kMaxUsers - 1);
  bool start = (gamepad.buttons & rex::input::X_INPUT_GAMEPAD_START) != 0;
  if (!state.shown || state.accepted || !state.answer) {
    state.held_since[user] = {};
    return false;
  }
  if (!start) {
    state.held_since[user] = {};
    return false;
  }
  if (state.held_since[user] == Clock::time_point{}) {
    state.held_since[user] = Clock::now();
  }
  // The game reads the pad at rest while START is held, so it does not pause; the hold is judged when the card is
  // drawn (the UI thread).
  return true;
}

}  // namespace rex::ui
