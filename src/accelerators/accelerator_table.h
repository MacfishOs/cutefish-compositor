// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/public/cpp/accelerators.h and
//           chromium/src/ash/accelerators/accelerator_table.h
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: the default bindings and the action categories, with
// keys as xkb keysyms. Deprecated, debug and developer accelerators are not
// carried over.
//
// Cutefish has no key-ack extension, so a client can never report that it did
// not consume a key: every accelerator here is handled before the focused
// window sees it. Defaults therefore prefer Meta and the dedicated media and
// brightness keys, which applications do not claim, over Ctrl-based
// combinations that ChromeOS can afford to offer the browser first.

#ifndef CUTEFISH_ACCELERATORS_ACCELERATOR_TABLE_H_
#define CUTEFISH_ACCELERATORS_ACCELERATOR_TABLE_H_

#include <array>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "accelerators/accelerator.h"
#include "accelerators/accelerator_actions.h"

namespace cutefish::accelerators {

struct AcceleratorData {
    bool triggerOnPress;
    uint32_t keysym;
    uint32_t modifiers;
    AcceleratorAction action;
    // The user may not change this particular combination, even though the
    // action itself stays editable.
    bool acceleratorLocked = false;
};

// There are three classes of accelerators, as in Ash:
//
// Reserved: handled whatever the focused window is, fullscreen included, and
// even while a window inhibits shortcuts.
// Preferred: handled whatever the focused window is, fullscreen included,
// unless a window inhibits shortcuts. Ash leaves these to a fullscreen window.
// Everything else: handled before the focused window unless it is fullscreen,
// since without a key-ack extension there is no post-target pass.
//
// As in Ash, a lone Meta opens the launcher on release; the
// AcceleratorLauncherStateMachine keeps Meta+L from doing it as well.
inline constexpr auto kAcceleratorData = std::to_array<AcceleratorData>({
        {false, XKB_KEY_Super_L, NoModifier, AcceleratorAction::ToggleAppList},
        {true, XKB_KEY_l, MetaDown, AcceleratorAction::LockScreen},

        // Window management.
        {true, XKB_KEY_Tab, AltDown, AcceleratorAction::CycleForwardMru},
        {true, XKB_KEY_Tab, AltDown | ShiftDown, AcceleratorAction::CycleBackwardMru},
        {true, XKB_KEY_equal, AltDown, AcceleratorAction::ToggleMaximized},
        {true, XKB_KEY_Up, MetaDown, AcceleratorAction::ToggleMaximized},
        {true, XKB_KEY_minus, AltDown, AcceleratorAction::WindowMinimize},
        {true, XKB_KEY_Down, MetaDown, AcceleratorAction::WindowMinimize},
        {true, XKB_KEY_f, MetaDown, AcceleratorAction::ToggleFullscreen},
        {true, XKB_KEY_bracketleft, AltDown, AcceleratorAction::WindowCycleSnapLeft},
        {true, XKB_KEY_Left, MetaDown, AcceleratorAction::WindowCycleSnapLeft},
        {true, XKB_KEY_bracketright, AltDown, AcceleratorAction::WindowCycleSnapRight},
        {true, XKB_KEY_Right, MetaDown, AcceleratorAction::WindowCycleSnapRight},
        {true, XKB_KEY_F4, AltDown, AcceleratorAction::CloseWindow},
        {true, XKB_KEY_m, MetaDown | AltDown, AcceleratorAction::MoveActiveWindowBetweenDisplays},

        // Desks, with Meta where upstream has Search.
        {true, XKB_KEY_bracketleft, MetaDown, AcceleratorAction::DesksActivateDeskLeft},
        {true, XKB_KEY_bracketright, MetaDown, AcceleratorAction::DesksActivateDeskRight},
        {true, XKB_KEY_bracketleft, MetaDown | ShiftDown,
         AcceleratorAction::DesksMoveActiveItemLeft},
        {true, XKB_KEY_bracketright, MetaDown | ShiftDown,
         AcceleratorAction::DesksMoveActiveItemRight},
        {true, XKB_KEY_equal, MetaDown | ShiftDown, AcceleratorAction::DesksNewDesk},
        {true, XKB_KEY_minus, MetaDown | ShiftDown, AcceleratorAction::DesksRemoveCurrentDesk},
        {true, XKB_KEY_1, MetaDown | ShiftDown, AcceleratorAction::DesksActivate0},
        {true, XKB_KEY_2, MetaDown | ShiftDown, AcceleratorAction::DesksActivate1},
        {true, XKB_KEY_3, MetaDown | ShiftDown, AcceleratorAction::DesksActivate2},
        {true, XKB_KEY_4, MetaDown | ShiftDown, AcceleratorAction::DesksActivate3},
        {true, XKB_KEY_5, MetaDown | ShiftDown, AcceleratorAction::DesksActivate4},
        {true, XKB_KEY_6, MetaDown | ShiftDown, AcceleratorAction::DesksActivate5},
        {true, XKB_KEY_7, MetaDown | ShiftDown, AcceleratorAction::DesksActivate6},
        {true, XKB_KEY_8, MetaDown | ShiftDown, AcceleratorAction::DesksActivate7},
        {true, XKB_KEY_a, MetaDown | ShiftDown, AcceleratorAction::DesksToggleAssignToAllDesks},

        // Screen capture.
        {true, XKB_KEY_Print, NoModifier, AcceleratorAction::StartCaptureMode},
        {true, XKB_KEY_Print, ShiftDown, AcceleratorAction::TakeScreenshot},
        {true, XKB_KEY_Print, MetaDown | ShiftDown, AcceleratorAction::TakePartialScreenshot},
        {true, XKB_KEY_Escape, ControlDown | MetaDown, AcceleratorAction::StopScreenRecording},
});

// The defaults under input::ShortcutConvention::Command. Command reaches the
// compositor as Meta, so MetaDown reads as Command: Command+Tab switches
// windows, Command+Space opens the launcher, Control+Command+Q locks, and
// Command+Q, Command+H and Command+Option+H are the application menu's Quit,
// Hide and Hide Others. Command+Shift+3 and 4 capture the screen and a
// selected part of it, onto the clipboard with Control as well, Command+Shift+5
// opens the capture tools and Control+Command+Escape stops a recording.
// Control+Left and Right step between desks and Control+1 to 8 go straight to
// one; with Shift as well Control+Left and Right take the window along.
// Control+Command+= and - add and remove a desk, and Control+Command+A puts the
// window on every desk. Everything else Command does goes to the application
// as Ctrl.
inline constexpr auto kCommandAcceleratorData = std::to_array<AcceleratorData>({
        {true, XKB_KEY_q, ControlDown | MetaDown, AcceleratorAction::LockScreen},
        {true, XKB_KEY_space, MetaDown, AcceleratorAction::ToggleAppList},

        // Window management.
        {true, XKB_KEY_Tab, MetaDown, AcceleratorAction::CycleForwardMru},
        {true, XKB_KEY_Tab, MetaDown | ShiftDown, AcceleratorAction::CycleBackwardMru},
        {true, XKB_KEY_m, MetaDown, AcceleratorAction::WindowMinimize},
        {true, XKB_KEY_q, MetaDown, AcceleratorAction::QuitApplication},
        {true, XKB_KEY_h, MetaDown, AcceleratorAction::HideApplication},
        {true, XKB_KEY_h, MetaDown | AltDown, AcceleratorAction::HideOtherApplications},
        {true, XKB_KEY_f, ControlDown | MetaDown, AcceleratorAction::ToggleFullscreen},
        {true, XKB_KEY_Up, ControlDown | MetaDown, AcceleratorAction::ToggleMaximized},
        {true, XKB_KEY_Left, ControlDown | MetaDown, AcceleratorAction::WindowCycleSnapLeft},
        {true, XKB_KEY_Right, ControlDown | MetaDown, AcceleratorAction::WindowCycleSnapRight},
        {true, XKB_KEY_m, ControlDown | MetaDown,
         AcceleratorAction::MoveActiveWindowBetweenDisplays},

        // Desks.
        {true, XKB_KEY_Left, ControlDown, AcceleratorAction::DesksActivateDeskLeft},
        {true, XKB_KEY_Right, ControlDown, AcceleratorAction::DesksActivateDeskRight},
        {true, XKB_KEY_Left, ControlDown | ShiftDown, AcceleratorAction::DesksMoveActiveItemLeft},
        {true, XKB_KEY_Right, ControlDown | ShiftDown,
         AcceleratorAction::DesksMoveActiveItemRight},
        {true, XKB_KEY_equal, ControlDown | MetaDown, AcceleratorAction::DesksNewDesk},
        {true, XKB_KEY_minus, ControlDown | MetaDown, AcceleratorAction::DesksRemoveCurrentDesk},
        {true, XKB_KEY_1, ControlDown, AcceleratorAction::DesksActivate0},
        {true, XKB_KEY_2, ControlDown, AcceleratorAction::DesksActivate1},
        {true, XKB_KEY_3, ControlDown, AcceleratorAction::DesksActivate2},
        {true, XKB_KEY_4, ControlDown, AcceleratorAction::DesksActivate3},
        {true, XKB_KEY_5, ControlDown, AcceleratorAction::DesksActivate4},
        {true, XKB_KEY_6, ControlDown, AcceleratorAction::DesksActivate5},
        {true, XKB_KEY_7, ControlDown, AcceleratorAction::DesksActivate6},
        {true, XKB_KEY_8, ControlDown, AcceleratorAction::DesksActivate7},
        {true, XKB_KEY_a, ControlDown | MetaDown, AcceleratorAction::DesksToggleAssignToAllDesks},

        // Screen capture.
        {true, XKB_KEY_3, MetaDown | ShiftDown, AcceleratorAction::TakeScreenshot},
        {true, XKB_KEY_3, ControlDown | MetaDown | ShiftDown,
         AcceleratorAction::TakeScreenshotToClipboard},
        {true, XKB_KEY_4, MetaDown | ShiftDown, AcceleratorAction::TakePartialScreenshot},
        {true, XKB_KEY_4, ControlDown | MetaDown | ShiftDown,
         AcceleratorAction::TakePartialScreenshotToClipboard},
        {true, XKB_KEY_5, MetaDown | ShiftDown, AcceleratorAction::StartCaptureMode},
        {true, XKB_KEY_Escape, ControlDown | MetaDown, AcceleratorAction::StopScreenRecording},
});

// Switching applications always switches, over a fullscreen game too.
inline constexpr std::array kPreferredActions = {
        AcceleratorAction::CycleBackwardMru,
        AcceleratorAction::CycleForwardMru,
};

// Actions that are always handled.
inline constexpr std::array kReservedActions = {
        AcceleratorAction::LockScreen,
};

// Actions allowed while the session is blocked: the lock screen and the login
// screen. Ash lists the device keys here; Cutefish has none yet.
inline constexpr std::array<AcceleratorAction, 0> kActionsAllowedAtLockScreen = {};

// Actions allowed while a system-modal dialog is up.
inline constexpr std::array kActionsAllowedAtModalWindow = {
        AcceleratorAction::LockScreen,
};

// Actions that require at least one window.
inline constexpr std::array kActionsNeedingWindow = {
        AcceleratorAction::DesksMoveActiveItemLeft,
        AcceleratorAction::DesksMoveActiveItemRight,
        AcceleratorAction::DesksToggleAssignToAllDesks,
        AcceleratorAction::CloseWindow,
        AcceleratorAction::MoveActiveWindowBetweenDisplays,
        AcceleratorAction::ToggleFullscreen,
        AcceleratorAction::ToggleMaximized,
        AcceleratorAction::WindowCycleSnapLeft,
        AcceleratorAction::WindowCycleSnapRight,
        AcceleratorAction::WindowMinimize,
        AcceleratorAction::QuitApplication,
        AcceleratorAction::HideApplication,
};

} // namespace cutefish::accelerators

#endif // CUTEFISH_ACCELERATORS_ACCELERATOR_TABLE_H_
