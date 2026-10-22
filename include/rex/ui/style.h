/**
 * @file        rex/ui/style.h
 *
 * @brief       Per-overlay styling that ImGuiStyle cannot express.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     ImGuiDrawer owns one Style; overlays read their own section
 *              through ImGuiDialog::imgui_drawer()->style(). Consumers patch
 *              it from ReXApp::OnConfigureStyle. Every default matches the
 *              value the corresponding overlay shipped with.
 */
#pragma once
#include <imgui.h>

namespace rex::ui {

// AchievementsStyle and ToastStyle are upstream's; on the recomp branch the achievement list and toast are drawn
// with DialogStyle instead (RECOMP.md), and these two are not read.
struct AchievementsStyle {
  ImVec4 unlocked_title{0.45f, 1.00f, 0.55f, 1.00f};
  ImVec4 unlocked_desc{0.70f, 0.85f, 0.72f, 1.00f};
  ImVec4 locked_title{0.78f, 0.80f, 0.84f, 1.00f};
  ImVec4 locked_desc{0.50f, 0.52f, 0.56f, 1.00f};
  ImVec4 badge_gamerscore{1.00f, 0.82f, 0.30f, 1.00f};
  ImVec4 row_unlocked_bg{0.16f, 0.30f, 0.18f, 0.55f};
  ImVec4 header_text{0.60f, 0.85f, 1.00f, 1.00f};
  ImVec4 progress_bar{0.30f, 0.80f, 0.40f, 1.00f};
  ImVec4 unlocked_icon_tint{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 locked_icon_tint{0.45f, 0.45f, 0.45f, 0.80f};
  ImVec2 window_padding{14.0f, 12.0f};
  ImVec2 item_spacing{8.0f, 6.0f};
  float icon_size = 44.0f;
  float row_rounding = 3.0f;
};

struct ToastStyle {
  /// Alpha is the fade envelope, applied on top of these at draw time.
  ImVec4 text{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 title{1.00f, 0.85f, 0.20f, 1.00f};
  float rounding = 6.0f;
  float background_alpha = 0.88f;
  float icon_size = 44.0f;
};

struct ConsoleStyle {
  /// Indexed by spdlog::level::level_enum: trace, debug, info, warn, err,
  /// critical, off.
  ImVec4 level[7] = {
      {0.5f, 0.5f, 0.5f, 1.0f},  // trace
      {0.4f, 0.9f, 0.9f, 1.0f},  // debug
      {1.0f, 1.0f, 1.0f, 1.0f},  // info
      {1.0f, 1.0f, 0.0f, 1.0f},  // warn
      {1.0f, 0.4f, 0.4f, 1.0f},  // err
      {1.0f, 0.0f, 0.0f, 1.0f},  // critical
      {1.0f, 1.0f, 1.0f, 1.0f},  // off
  };
  ImVec4 completion_bg{0.10f, 0.10f, 0.10f, 0.95f};
  ImVec4 completion_highlight{0.235f, 0.353f, 0.549f, 0.784f};
};

struct DebugOverlayStyle {
  ImVec4 muted_text{1.00f, 1.00f, 1.00f, 0.50f};
};

struct SettingsStyle {
  ImVec4 warning{1.00f, 0.80f, 0.20f, 1.00f};
  /// Cvar lifecycle badges: [live], [restart], [init-only].
  ImVec4 lifecycle_live{0.40f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_restart{1.00f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_init_only{1.00f, 0.40f, 0.40f, 1.00f};
  ImVec4 lifecycle_unknown{1.00f, 1.00f, 1.00f, 1.00f};
};

/// The recomp branch's dialogs (the achievement toast and list, the quit prompt; RECOMP.md): a dark card with an
/// accent color, in Barlow (ImGuiDrawer::Font). Colors at full opacity: the dialogs fade them.
struct DialogStyle {
  ImVec4 background{0.082f, 0.102f, 0.129f, 0.97f};  // #151A21
  ImVec4 raised{0.125f, 0.149f, 0.184f, 1.00f};      // #20262F: icon wells, the progress track, buttons
  ImVec4 hover{0.180f, 0.212f, 0.259f, 1.00f};       // #2E3642: a button under the mouse
  ImVec4 border{1.00f, 1.00f, 1.00f, 0.08f};
  ImVec4 text{0.914f, 0.925f, 0.945f, 1.00f};        // #E9ECF1
  ImVec4 muted{0.639f, 0.675f, 0.725f, 1.00f};       // #A3ACB9
  ImVec4 dim{0.486f, 0.525f, 0.584f, 1.00f};         // #7C8695
  ImVec4 accent{0.878f, 0.663f, 0.231f, 1.00f};      // #E0A93B
  ImVec4 on_accent{0.090f, 0.071f, 0.039f, 1.00f};   // #17120A
  ImVec4 success{0.357f, 0.769f, 0.537f, 1.00f};     // #5BC489
  ImVec4 shadow{0.00f, 0.00f, 0.00f, 0.35f};
  ImVec4 dim_background{0.00f, 0.00f, 0.00f, 0.59f};  // behind a modal dialog
  float rounding = 12.0f;  // in pixels of a 720-line screen
};

struct Style {
  AchievementsStyle achievements;
  ToastStyle toast;
  ConsoleStyle console;
  DebugOverlayStyle debug;
  SettingsStyle settings;
  DialogStyle dialog;
};

}  // namespace rex::ui
