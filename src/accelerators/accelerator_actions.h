// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/public/cpp/accelerator_actions.h
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: the launcher, the lock screen, the window actions,
// the application menu's Quit, Hide and Hide Others, desks and screen capture.
// Device keys (volume, brightness, media) wait for the shell's OSD; browser,
// IME, accessibility, tablet and debug actions have nothing to act on.

#ifndef CUTEFISH_ACCELERATORS_ACCELERATOR_ACTIONS_H_
#define CUTEFISH_ACCELERATORS_ACCELERATOR_ACTIONS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include <QString>

namespace cutefish::accelerators {

// Persisted and exposed over D-Bus by name, never by value, so the order here
// is free to change.
enum class AcceleratorAction : uint32_t {
    // General.
    ToggleAppList,
    LockScreen,

    // Windows.
    CycleForwardMru,
    CycleBackwardMru,
    ToggleMaximized,
    WindowMinimize,
    // The application menu's Quit, Hide and Hide Others.
    QuitApplication,
    HideApplication,
    HideOtherApplications,
    ToggleFullscreen,
    WindowCycleSnapLeft,
    WindowCycleSnapRight,
    CloseWindow,
    MoveActiveWindowBetweenDisplays,
    // Desks.
    DesksActivateDeskLeft,
    DesksActivateDeskRight,
    DesksMoveActiveItemLeft,
    DesksMoveActiveItemRight,
    DesksNewDesk,
    DesksRemoveCurrentDesk,
    DesksActivate0,
    DesksActivate1,
    DesksActivate2,
    DesksActivate3,
    DesksActivate4,
    DesksActivate5,
    DesksActivate6,
    DesksActivate7,
    DesksToggleAssignToAllDesks,

    // Screen capture, performed by the shell.
    TakeScreenshot,
    TakeScreenshotToClipboard,
    TakePartialScreenshot,
    TakePartialScreenshotToClipboard,
    StartCaptureMode,
    StopScreenRecording,
};

using AcceleratorActionId = uint32_t;

constexpr AcceleratorActionId toActionId(AcceleratorAction action)
{
    return static_cast<AcceleratorActionId>(action);
}

// The kebab-case identifier used in preferences and on the bus.
QString acceleratorActionName(AcceleratorAction action);
std::optional<AcceleratorAction> acceleratorActionFromName(const QString &name);

// Every action, in declaration order. An action need not have a default
// binding to exist: it is then listed with none until the user adds one.
std::vector<AcceleratorAction> allAcceleratorActions();

} // namespace cutefish::accelerators

#endif // CUTEFISH_ACCELERATORS_ACCELERATOR_ACTIONS_H_
