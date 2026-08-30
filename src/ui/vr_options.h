/**
 * @file    ui/vr_options.h
 * @brief   In-headset VR options panel: camera mode, comfort, HUD, scale.
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include <rex/types.h>

namespace rex::ui {
class ImGuiDrawer;
}

namespace bd::ui {

// Every VR comfort and immersion setting already exists as a cvar and none of
// them could be reached without editing a TOML on the device - which for a
// standalone headset means a PC, adb, and a relaunch. This is the surface.
//
// Driven from the Touch controllers rather than a mouse: the panel is only
// useful in the place where there is no mouse. xr_pad.cpp routes input here
// while it is open and feeds the guest a neutral pad, so the game does not
// walk the character around underneath the menu.
namespace vr_options {

// Both grips held with both triggers - a shape the game never asks for.
bool IsOpen();
void SetOpen(bool open);
void Toggle();

// -1 / +1. Move changes the selected row, Adjust changes its value.
void Move(int delta);
void Adjust(int delta);

// Number of rows, for the caller that wants to know without drawing.
u32 RowCount();

} // namespace vr_options

// Registers the panel with the drawer. Safe to call repeatedly.
void EnsureVrOptionsPanel(rex::ui::ImGuiDrawer *drawer);
void DestroyVrOptionsPanel();

} // namespace bd::ui
