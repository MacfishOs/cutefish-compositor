// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_CONTROLCENTER_CONTROL_CENTER_VIEW_H_
#define CUTEFISH_SHELL_CONTROLCENTER_CONTROL_CENTER_VIEW_H_

#include <functional>

#include "base/timer.h"
#include "ui/view.h"

namespace cutefish::ui {
class Label;
}

namespace cutefish::shell {

class CardItem;
class ControlCenterServices;
class DeviceUsersTile;
class HeaderButton;
class MediaTile;
class PanelRow;
class TileSlider;

// The control centre's view matching reference QML design.
class ControlCenterView : public ui::View
{
public:
    static constexpr double kWidth = 390;
    static constexpr double kGap = 10;

    enum class SubPanel { Main, Wifi, Bluetooth };

    explicit ControlCenterView(ControlCenterServices &services);
    ~ControlCenterView() override;

    void refresh();
    // Takes its text again after the session language changed.
    void retranslate();
    // The window height at `scale`: the layout's, on whole device pixels.
    double windowHeight(double scale) const;

    SubPanel subPanel() const { return m_subPanel; }
    void setSubPanel(SubPanel panel) { m_subPanel = panel; requestLayout(); refresh(); }

    // Top 2x2 Cards
    CardItem *wirelessCard() const { return m_wireless; }
    CardItem *bluetoothCard() const { return m_bluetooth; }
    CardItem *wirelessShareCard() const { return m_wirelessShare; }
    CardItem *kdeConnectCard() const { return m_kdeConnect; }

    // Quick Toggles (returning CardItem* for test compatibility)
    CardItem *darkModeCard() const { return m_darkModeToggle; }
    CardItem *doNotDisturbCard() const { return m_doNotDisturbToggle; }
    CardItem *cameraCard() const { return m_cameraToggle; }

    MediaTile *media() const { return m_media; }
    TileSlider *brightnessSlider() const { return m_brightness; }
    TileSlider *soundSlider() const { return m_sound; }

    // Actions
    std::function<void()> onLockRequested;
    std::function<void()> onSettingsRequested;
    std::function<void()> onScreenshotRequested;
    std::function<void(int action)> onPowerActionRequested; // 1: logout, 2: reboot, 3: shutdown

    std::function<void()> onHideRequested;
    std::function<void()> onHeightChanged;

protected:
    void layout() override;

private:
    double layoutHeight() const;

    ControlCenterServices &m_services;

    // Header
    ui::Label *m_title;
    HeaderButton *m_lockButton;
    HeaderButton *m_settingsButton;
    HeaderButton *m_backButton;

    // Privacy Dot Users
    DeviceUsersTile *m_users;

    // 2x2 Top Cards
    CardItem *m_wireless;
    CardItem *m_bluetooth;
    CardItem *m_wirelessShare;
    CardItem *m_kdeConnect;

    // Media
    MediaTile *m_media;

    // Sliders
    TileSlider *m_brightness;
    TileSlider *m_sound;

    // 2x5 Quick Toggles
    CardItem *m_powerModeToggle;
    CardItem *m_nightShiftToggle;
    CardItem *m_captureToggle;
    CardItem *m_doNotDisturbToggle;
    CardItem *m_darkModeToggle;
    CardItem *m_keyLightToggle;
    CardItem *m_locationToggle;
    CardItem *m_logoutToggle;
    CardItem *m_restartToggle;
    CardItem *m_powerOffToggle;

    // Additional fallback card for camera test accessor if needed
    CardItem *m_cameraToggle;

    base::Timer m_brightnessTimer;
    bool m_darkModeShown = false;

    // Quick toggle states
    bool m_wirelessShareEnabled = false;
    bool m_nightLightEnabled = false;
    int m_keyLightLevel = 0; // 0: Off, 1: Low, 2: High
    bool m_locationEnabled = false;
    SubPanel m_subPanel = SubPanel::Main;
    std::vector<PanelRow*> m_subRows;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_CONTROLCENTER_CONTROL_CENTER_VIEW_H_
