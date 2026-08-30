/**
 * @file    ui/vr_options.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "ui/vr_options.h"

#include <algorithm>
#include <format>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>

#include "core/logging.h"
#include <rex/types.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

REXCVAR_DECLARE(bool, bd_vr_menu);
REXCVAR_DECLARE(i32, bd_vr_camera_mode);
REXCVAR_DECLARE(f64, bd_vr_world_scale);
REXCVAR_DECLARE(bool, bd_vr_battle_diorama);
REXCVAR_DECLARE(f64, bd_vr_diorama_height);
REXCVAR_DECLARE(bool, bd_vr_snap_turn);
REXCVAR_DECLARE(f64, bd_vr_turn_degrees);
REXCVAR_DECLARE(bool, bd_vr_comfort_vignette);
REXCVAR_DECLARE(i32, bd_vr_hud_mode);
REXCVAR_DECLARE(f64, bd_vr_hud_distance);
REXCVAR_DECLARE(f64, bd_vr_hud_scale);
REXCVAR_DECLARE(i32, bd_vr_cutscene_policy);
REXCVAR_DECLARE(f64, bd_vr_third_offset_y);
REXCVAR_DECLARE(f64, bd_vr_third_offset_z);

namespace bd::ui {

namespace {

// Row kinds are deliberately tiny: this panel is driven by a thumbstick, so
// every control is "left decreases, right increases". No sliders, no text
// entry, nothing that needs a pointer.
enum class Kind { Choice, Toggle, Number };

struct Row {
  const char *label;
  const char *help;
  Kind kind;
  const char *const *choices;
  int choice_count;
  double step, lo, hi;
  int (*get_i)();
  void (*set_i)(int);
  double (*get_f)();
  void (*set_f)(double);
};

const char *const kCameraModes[] = {"First person", "Third person", "Diorama",
                                    "Flat screen"};
const char *const kHudModes[] = {"Follows head", "Pinned in world", "Hidden"};
const char *const kCutscenePolicies[] = {"Keep head in control",
                                         "Diorama for the scene",
                                         "As authored (may cause sickness)"};

#define BD_ROW_I(cv)                                                           \
  []() -> int { return static_cast<int>(REXCVAR_GET(cv)); },                   \
      [](int v) { REXCVAR_SET(cv, v); }, nullptr, nullptr
#define BD_ROW_F(cv)                                                           \
  nullptr, nullptr, []() -> double { return REXCVAR_GET(cv); },                \
                    [](double v) { REXCVAR_SET(cv, v); }

const Row kRows[] = {
    {"Camera", "How you sit in the world. Diorama looks down on the scene like "
               "a model; third person rides behind the party.",
     Kind::Choice, kCameraModes, 4, 0, 0, 0, BD_ROW_I(bd_vr_camera_mode)},
    {"World scale",
     "Above 1 makes the world larger and you smaller. Diorama's main knob.",
     Kind::Number, nullptr, 0, 0.1, 0.1, 10.0, BD_ROW_F(bd_vr_world_scale)},
    {"Diorama in battle",
     "A battle is a stationary set piece, so a follow camera has nothing to "
     "follow and spends the fight fighting the game's own camera.",
     Kind::Toggle, nullptr, 0, 0, 0, 0, BD_ROW_I(bd_vr_battle_diorama)},
    {"Diorama height", "Metres above the scene in diorama mode.", Kind::Number,
     nullptr, 0, 0.1, 0.0, 20.0, BD_ROW_F(bd_vr_diorama_height)},
    {"Camera height",
     "Third person: height above the character, in game units. 100 units is a "
     "metre.",
     Kind::Number, nullptr, 0, 10.0, -2000.0, 2000.0,
     BD_ROW_F(bd_vr_third_offset_y)},
    {"Camera distance",
     "Third person: how far behind the character, in game units.",
     Kind::Number, nullptr, 0, 25.0, -2000.0, 2000.0,
     BD_ROW_F(bd_vr_third_offset_z)},
    {"Snap turn",
     "Turn in steps rather than sweeping. Far more comfortable for most "
     "people.",
     Kind::Toggle, nullptr, 0, 0, 0, 0, BD_ROW_I(bd_vr_snap_turn)},
    {"Turn step", "Degrees per snap.", Kind::Number, nullptr, 0, 5.0, 5.0, 90.0,
     BD_ROW_F(bd_vr_turn_degrees)},
    {"Comfort vignette",
     "Narrows the view while you are moving. Costs peripheral vision, buys a "
     "great deal of comfort.",
     Kind::Toggle, nullptr, 0, 0, 0, 0, BD_ROW_I(bd_vr_comfort_vignette)},
    {"HUD", "Where the 2D layer sits.", Kind::Choice, kHudModes, 3, 0, 0, 0,
     BD_ROW_I(bd_vr_hud_mode)},
    {"HUD distance",
     "Metres from your eye to the 2D layer. Closer than about a metre is "
     "uncomfortable to focus on.",
     Kind::Number, nullptr, 0, 0.25, 0.5, 20.0, BD_ROW_F(bd_vr_hud_distance)},
    {"HUD size", "Size of the 2D layer.", Kind::Number, nullptr, 0, 0.1, 0.1,
     5.0, BD_ROW_F(bd_vr_hud_scale)},
    {"Cutscenes",
     "What scripted camera moves are allowed to do to your head. Honouring "
     "them as authored will make most people ill.",
     Kind::Choice, kCutscenePolicies, 3, 0, 0, 0,
     BD_ROW_I(bd_vr_cutscene_policy)},
};
#undef BD_ROW_I
#undef BD_ROW_F

constexpr int kRowCount = static_cast<int>(std::size(kRows));

bool g_open = false;
int g_selected = 0;

std::string ValueText(const Row &r) {
  switch (r.kind) {
  case Kind::Choice: {
    const int v = std::clamp(r.get_i(), 0, r.choice_count - 1);
    return r.choices[v];
  }
  case Kind::Toggle:
    return r.get_i() ? "On" : "Off";
  case Kind::Number:
  default:
    return std::format("{:.2f}", r.get_f());
  }
}

class VrOptionsPanel final : public rex::ui::ImGuiDialog {
public:
  explicit VrOptionsPanel(rex::ui::ImGuiDrawer *drawer)
      : rex::ui::ImGuiDialog(drawer) {}

protected:
  void OnDraw(ImGuiIO &io) override {
    // Through IsOpen(), not g_open: the cvar override has to draw too.
    if (!vr_options::IsOpen())
      return;

    // Centred and large. This is read through a headset lens at whatever the
    // composited panel's resolution happens to be, where small text is simply
    // unreadable - the first headset session failed on readability.
    const ImVec2 size(std::min(io.DisplaySize.x * 0.62f, 760.0f),
                      std::min(io.DisplaySize.y * 0.78f, 620.0f));
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::Begin("VR options", nullptr,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::End();
      return;
    }

    // Once. The composited capture path returns an all-black frame on device,
    // so there is no way to see this panel in a screenshot - this line is the
    // only evidence available that it draws at all.
    static bool told = false;
    if (!told) {
      told = true;
      BD_INFO("[vr-options] panel drawing, {} rows, display {}x{}",
              kRowCount, static_cast<int>(io.DisplaySize.x),
              static_cast<int>(io.DisplaySize.y));
    }

    ImGui::TextUnformatted(
        "Stick up/down to choose, left/right to change. B closes.");
    ImGui::Separator();
    ImGui::Spacing();

    for (int i = 0; i < kRowCount; ++i) {
      const Row &r = kRows[i];
      const bool sel = (i == g_selected);
      if (sel)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
      ImGui::Text("%s %-18s  %s", sel ? ">" : " ", r.label,
                  ValueText(r).c_str());
      if (sel)
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("%s", kRows[g_selected].help);
    ImGui::End();
  }
};

std::unique_ptr<VrOptionsPanel> g_panel;

} // namespace

namespace vr_options {

bool IsOpen() {
  // The cvar is an override, not the state: it opens the panel for an
  // unattended capture without the controller gesture having to fire.
  return g_open || REXCVAR_GET(bd_vr_menu);
}
void SetOpen(bool open) { g_open = open; }
void Toggle() { g_open = !g_open; }
u32 RowCount() { return static_cast<u32>(kRowCount); }

void Move(int delta) {
  g_selected = (g_selected + delta % kRowCount + kRowCount) % kRowCount;
}

void Adjust(int delta) {
  const Row &r = kRows[g_selected];
  switch (r.kind) {
  case Kind::Choice: {
    const int n = r.choice_count;
    r.set_i((r.get_i() + delta % n + n) % n);
    break;
  }
  case Kind::Toggle:
    r.set_i(r.get_i() ? 0 : 1);
    break;
  case Kind::Number:
    r.set_f(std::clamp(r.get_f() + r.step * delta, r.lo, r.hi));
    break;
  }
}

} // namespace vr_options

void EnsureVrOptionsPanel(rex::ui::ImGuiDrawer *drawer) {
  if (!g_panel && drawer)
    g_panel = std::make_unique<VrOptionsPanel>(drawer);
}

void DestroyVrOptionsPanel() { g_panel.reset(); }

} // namespace bd::ui
