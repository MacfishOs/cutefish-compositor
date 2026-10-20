// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/controlcenter/control_center_view.h"

#include <cmath>
#include <vector>

#include "base/i18n.h"
#include "shell/common/text_match.h"
#include "shell/controlcenter/control_center_services.h"
#include "shell/controlcenter/level_slider.h"
#include "shell/controlcenter/panel_style.h"
#include "shell/controlcenter/status_panel.h"
#include "shell/controlcenter/tiles.h"
#include "ui/label.h"
#include "ui/theme.h"
#include "ui/window.h"

using cutefish::i18n::tr;

namespace cutefish::shell {

using BluetoothDevice = ControlCenterServices::BluetoothDevice;

static constexpr std::chrono::milliseconds kBrightnessDelay {100};

static double windowScale(const ui::View *view)
{
    const ui::Window *window = view->window();
    return window && window->scale() > 0 ? window->scale() : 1;
}

ControlCenterView::ControlCenterView(ControlCenterServices &services)
    : m_services(services)
{
    m_title = addChild<ui::Label>(tr("Control Center"));
    m_title->setFontSize(15.0);
    m_title->setElide(ui::Label::Elide::Right);

    m_lockButton = addChild<HeaderButton>();
    m_lockButton->setIcon(panel::artwork("system-lock-screen", panel::dark()));
    m_lockButton->onClicked = [this] {
        if (onLockRequested)
            onLockRequested();
    };

    m_settingsButton = addChild<HeaderButton>();
    m_settingsButton->setIcon(panel::artwork("settings", panel::dark()));
    m_settingsButton->onClicked = [this] {
        if (onSettingsRequested)
            onSettingsRequested();
        else if (onHideRequested)
            onHideRequested();
    };

    m_backButton = addChild<HeaderButton>();
    m_backButton->setIcon(panel::artwork("down", panel::dark()));
    m_backButton->setVisible(false);
    m_backButton->onClicked = [this] {
        m_subPanel = SubPanel::Main;
        requestLayout();
        refresh();
    };

    m_users = addChild<DeviceUsersTile>();

    // 2x2 Top Cards
    m_wireless = addChild<CardItem>();
    m_wireless->setMode(CardItem::Mode::TopCard);
    m_wireless->setArtwork("wifi");
    m_wireless->setTitle(tr("Wi-Fi"));
    m_wireless->setActiveColor(ui::Color::rgb(0x007AFF));
    m_wireless->setShowChevron(true);
    m_wireless->onClicked = [this] { m_services.enableWireless(!m_wireless->isChecked()); };
    m_wireless->onPressAndHold = [this] {
        if (onHideRequested)
            onHideRequested();
        m_services.openSettings({"-m", "wlan"});
    };
    m_wireless->onChevronClicked = [this] {
        m_subPanel = SubPanel::Wifi;
        m_services.requestScan();
        requestLayout();
        refresh();
    };

    m_bluetooth = addChild<CardItem>();
    m_bluetooth->setMode(CardItem::Mode::TopCard);
    m_bluetooth->setArtwork("bluetooth-symbolic");
    m_bluetooth->setTitle(tr("Bluetooth"));
    m_bluetooth->setActiveColor(ui::Color::rgb(0x007AFF));
    m_bluetooth->setShowChevron(true);
    m_bluetooth->onClicked = [this] { m_services.setBluetoothEnabled(!m_services.bluetoothEnabled()); };
    m_bluetooth->onPressAndHold = [this] {
        if (onHideRequested)
            onHideRequested();
        m_services.openSettings({"-m", "bluetooth"});
    };
    m_bluetooth->onChevronClicked = [this] {
        m_subPanel = SubPanel::Bluetooth;
        requestLayout();
        refresh();
    };

    m_wirelessShare = addChild<CardItem>();
    m_wirelessShare->setMode(CardItem::Mode::TopCard);
    m_wirelessShare->setArtwork("wireless-share");
    m_wirelessShare->setTitle(tr("Wireless Share"));
    m_wirelessShare->setActiveColor(ui::Color::rgb(0x007AFF));
    m_wirelessShare->setShowChevron(false);
    m_wirelessShare->onClicked = [this] {
        m_wirelessShareEnabled = !m_wirelessShareEnabled;
        refresh();
    };

    m_kdeConnect = addChild<CardItem>();
    m_kdeConnect->setMode(CardItem::Mode::TopCard);
    m_kdeConnect->setArtwork("phone");
    m_kdeConnect->setTitle(tr("KDE Connect"));
    m_kdeConnect->setActiveColor(ui::Color::rgb(0x34C759));
    m_kdeConnect->setShowChevron(true);

    // Media Tile
    m_media = addChild<MediaTile>();
    m_media->onPrevious = [this] { m_services.mediaPrevious(); };
    m_media->onPlay = [this] { m_services.mediaPlay(); };
    m_media->onPause = [this] { m_services.mediaPause(); };
    m_media->onNext = [this] { m_services.mediaNext(); };

    // Sliders
    // TRANSLATORS: the title of the screen brightness slider.
    m_brightness = addChild<TileSlider>(tr("Display"));
    m_brightness->slider()->setRange(1, 100, 1);
    m_brightness->slider()->setIcon(panel::artwork("brightness", false));
    m_brightnessTimer.setSingleShot(true);
    m_brightnessTimer.setInterval(kBrightnessDelay);
    m_brightnessTimer.onTimeout = [this] {
        m_services.setBrightness(int(std::lround(m_brightness->slider()->visualValue())));
    };
    m_brightness->slider()->onMoved = [this](double) { m_brightnessTimer.start(); };

    m_sound = addChild<TileSlider>(tr("Sound"));
    m_sound->slider()->setRange(0, 1, 0.01);
    m_sound->slider()->onMoved = [this](double value) {
        if (!m_services.hasSink())
            return;
        m_services.setVolume(value);
        m_services.setMuted(value == 0);
    };

    // 2x5 Quick Toggles
    m_powerModeToggle = addChild<CardItem>();
    m_powerModeToggle->setMode(CardItem::Mode::QuickToggle);
    m_powerModeToggle->onClicked = [this] {
        const std::string profile = m_services.powerProfile();
        std::string next = "balanced";
        if (profile == "balanced")
            next = "performance";
        else if (profile == "performance")
            next = "power-saver";
        else
            next = "balanced";
        m_services.setPowerProfile(next);
    };

    m_nightShiftToggle = addChild<CardItem>();
    m_nightShiftToggle->setMode(CardItem::Mode::QuickToggle);
    m_nightShiftToggle->setTitle(tr("Night"));
    m_nightShiftToggle->setActiveColor(ui::Color::rgb(0xFF9500));
    m_nightShiftToggle->onClicked = [this] {
        m_services.setNightLightEnabled(!m_services.nightLightEnabled());
    };

    m_captureToggle = addChild<CardItem>();
    m_captureToggle->setMode(CardItem::Mode::QuickToggle);
    m_captureToggle->setTitle(tr("Capture"));
    m_captureToggle->setArtwork("screenshot");
    m_captureToggle->onClicked = [this] {
        if (onScreenshotRequested)
            onScreenshotRequested();
    };

    m_doNotDisturbToggle = addChild<CardItem>();
    m_doNotDisturbToggle->setMode(CardItem::Mode::QuickToggle);
    m_doNotDisturbToggle->setTitle(tr("Focus"));
    m_doNotDisturbToggle->setActiveColor(ui::Color::rgb(0x5856D6));
    m_doNotDisturbToggle->onClicked = [this] {
        m_services.setDoNotDisturb(!m_services.doNotDisturb());
    };

    m_darkModeToggle = addChild<CardItem>();
    m_darkModeToggle->setMode(CardItem::Mode::QuickToggle);
    m_darkModeToggle->setActiveColor(ui::Color::rgb(0x007AFF));
    m_darkModeToggle->onClicked = [this] {
        m_services.setDarkMode(!ui::Theme::current().darkMode);
    };

    m_keyLightToggle = addChild<CardItem>();
    m_keyLightToggle->setMode(CardItem::Mode::QuickToggle);
    m_keyLightToggle->setActiveColor(ui::Color::rgb(0x007AFF));
    m_keyLightToggle->onClicked = [this] {
        const int nextLevel = (m_services.keyboardBacklightLevel() + 1) % 3;
        m_services.setKeyboardBacklightLevel(nextLevel);
    };

    m_cameraToggle = addChild<CardItem>();
    m_cameraToggle->setMode(CardItem::Mode::QuickToggle);
    m_cameraToggle->setTitle(tr("Camera"));
    m_cameraToggle->setArtwork("camera");
    m_cameraToggle->setActiveColor(ui::Color::rgb(0x007AFF));
    m_cameraToggle->onClicked = [this] {
        m_services.setCameraEnabled(!m_services.cameraEnabled());
    };

    m_logoutToggle = addChild<CardItem>();
    m_logoutToggle->setMode(CardItem::Mode::QuickToggle);
    m_logoutToggle->setTitle(tr("Logout"));
    m_logoutToggle->setArtwork("system-log-out");
    m_logoutToggle->onClicked = [this] {
        if (onPowerActionRequested)
            onPowerActionRequested(1);
    };

    m_restartToggle = addChild<CardItem>();
    m_restartToggle->setMode(CardItem::Mode::QuickToggle);
    m_restartToggle->setTitle(tr("Restart"));
    m_restartToggle->setArtwork("system-reboot");
    m_restartToggle->onClicked = [this] {
        if (onPowerActionRequested)
            onPowerActionRequested(2);
    };

    m_powerOffToggle = addChild<CardItem>();
    m_powerOffToggle->setMode(CardItem::Mode::QuickToggle);
    m_powerOffToggle->setTitle(tr("Power Off"));
    m_powerOffToggle->setArtwork("system-shutdown");
    m_powerOffToggle->setActiveColor(ui::Color::rgb(0xFF3B30));
    m_powerOffToggle->setChecked(true);
    m_powerOffToggle->onClicked = [this] {
        if (onPowerActionRequested)
            onPowerActionRequested(3);
    };

    m_locationToggle = addChild<CardItem>();
    m_locationToggle->setMode(CardItem::Mode::QuickToggle);
    m_locationToggle->setTitle(tr("Location"));
    m_locationToggle->setArtwork("location-mode");
    m_locationToggle->setActiveColor(ui::Color::rgb(0x007AFF));
    m_locationToggle->setVisible(false);

    refresh();
}

ControlCenterView::~ControlCenterView() = default;

void ControlCenterView::retranslate()
{
    m_title->setText(tr("Control Center"));
    m_wireless->setTitle(tr("Wi-Fi"));
    m_bluetooth->setTitle(tr("Bluetooth"));
    m_wirelessShare->setTitle(tr("Wireless Share"));
    m_kdeConnect->setTitle(tr("KDE Connect"));
    m_nightShiftToggle->setTitle(tr("Night"));
    m_captureToggle->setTitle(tr("Capture"));
    m_doNotDisturbToggle->setTitle(tr("Focus"));
    m_cameraToggle->setTitle(tr("Camera"));
    m_logoutToggle->setTitle(tr("Logout"));
    m_restartToggle->setTitle(tr("Restart"));
    m_powerOffToggle->setTitle(tr("Power Off"));
    m_locationToggle->setTitle(tr("Location"));
    // TRANSLATORS: the title of the screen brightness slider.
    m_brightness->setTitle(tr("Display"));
    m_sound->setTitle(tr("Sound"));
    m_users->retranslate();
    refresh();
}

void ControlCenterView::refresh()
{
    const double before = layoutHeight();
    const bool dark = ui::Theme::current().darkMode;
    const bool isMain = m_subPanel == SubPanel::Main;

    m_backButton->setVisible(!isMain);
    m_backButton->setIcon(panel::artwork("chevron-left", dark));
    m_lockButton->setVisible(isMain);
    m_settingsButton->setVisible(isMain);

    if (m_subPanel == SubPanel::Wifi)
        m_title->setText(tr("Wi-Fi Networks"));
    else if (m_subPanel == SubPanel::Bluetooth)
        m_title->setText(tr("Bluetooth Devices"));
    else
        m_title->setText(tr("Control Center"));

    m_lockButton->setIcon(panel::artwork("system-lock-screen", dark));
    m_settingsButton->setIcon(panel::artwork("settings", dark));

    if (m_subPanel == SubPanel::Wifi) {
        std::vector<WirelessNetwork> networks = m_services.wirelessNetworks();
        while (m_subRows.size() > networks.size()) {
            takeChild(m_subRows.back());
            m_subRows.pop_back();
        }
        while (m_subRows.size() < networks.size()) {
            PanelRow *row = addChild<PanelRow>();
            m_subRows.push_back(row);
        }
        for (size_t i = 0; i < networks.size(); ++i) {
            const WirelessNetwork &net = networks[i];
            PanelRow *row = m_subRows[i];
            row->setVisible(true);
            bool isLocked = net.security != WirelessNetwork::Security::None && net.security != WirelessNetwork::Security::Unknown;
            if (isLocked)
                row->setArtwork(panel::artwork("network-wireless-locked", dark));
            else
                row->setArtwork(panel::artwork(!net.icon.empty() ? net.icon : "network-wireless-connected-100", dark));
            row->setText(!net.name.empty() ? net.name : tr("Wi-Fi Network"));
            bool connected = net.state == WirelessNetwork::State::Activated;
            row->setActive(connected);
            row->setBold(connected);
            row->setBusy(net.state == WirelessNetwork::State::Activating);
            row->setLocked(isLocked);
            row->onClicked = [this, net] {
                m_services.activateNetwork(net);
            };
        }
    } else if (m_subPanel == SubPanel::Bluetooth) {
        std::vector<BluetoothDevice> devices = m_services.bluetoothDevices();
        while (m_subRows.size() > devices.size()) {
            takeChild(m_subRows.back());
            m_subRows.pop_back();
        }
        while (m_subRows.size() < devices.size()) {
            PanelRow *row = addChild<PanelRow>();
            m_subRows.push_back(row);
        }
        for (size_t i = 0; i < devices.size(); ++i) {
            const BluetoothDevice &dev = devices[i];
            PanelRow *row = m_subRows[i];
            row->setVisible(true);
            std::string iconName = dev.icon;
            if (iconName.empty() || iconName == "bluetooth" || iconName == "bluetooth-symbolic") {
                std::string lowerName = toLower(dev.name);
                if (lowerName.find("airpod") != std::string::npos || lowerName.find("bud") != std::string::npos ||
                    lowerName.find("headset") != std::string::npos || lowerName.find("headphone") != std::string::npos ||
                    lowerName.find("earphone") != std::string::npos || lowerName.find("audio") != std::string::npos ||
                    lowerName.find("sony") != std::string::npos || lowerName.find("jbl") != std::string::npos ||
                    lowerName.find("bose") != std::string::npos || lowerName.find("beats") != std::string::npos) {
                    iconName = "audio-headset";
                } else {
                    iconName = "bluetooth-symbolic";
                }
            }
            row->setArtwork(panel::artwork(iconName, dark));
            row->setText(!dev.name.empty() ? dev.name : dev.address);
            row->setActive(dev.connected);
            row->setBold(dev.connected);
            row->setBusy(dev.busy);
            row->setDetail(dev.connected ? tr("Connected") : dev.paired ? tr("Paired") : tr("Not Connected"));
            row->onClicked = [this, dev] {
                if (dev.connected)
                    m_services.disconnectBluetoothDevice(dev.address);
                else
                    m_services.connectBluetoothDevice(dev.address);
            };
        }
    } else {
        for (PanelRow *row : m_subRows)
            row->setVisible(false);
    }

    m_users->setVisible(isMain && !m_services.deviceUsers().empty());
    m_users->setUsers(m_services.deviceUsers());

    m_wireless->setVisible(isMain && m_services.wirelessHardwareEnabled());
    m_wireless->setChecked(m_services.wirelessEnabled());
    m_wireless->setArtwork(m_services.wirelessEnabled() ? "wifi" : "wifi_disabled");
    const std::string network = m_services.wirelessName();
    m_wireless->setStatus(!m_wireless->isChecked() ? tr("Off") : !network.empty() ? network : tr("On"));

    m_bluetooth->setVisible(isMain && m_services.bluetoothAvailable());
    const bool btEnabled = m_services.bluetoothEnabled();
    m_bluetooth->setChecked(btEnabled);
    m_bluetooth->setArtwork(btEnabled ? "bluetooth-symbolic" : "bluetooth-off");
    if (!btEnabled) {
        m_bluetooth->setStatus(tr("Off"));
    } else {
        std::string connectedName;
        for (const auto &dev : m_services.bluetoothDevices()) {
            if (dev.connected) {
                connectedName = !dev.name.empty() ? dev.name : dev.address;
                break;
            }
        }
        m_bluetooth->setStatus(!connectedName.empty() ? connectedName : tr("On"));
    }

    m_wirelessShare->setVisible(isMain);
    m_wirelessShare->setChecked(m_wirelessShareEnabled);
    m_wirelessShare->setStatus(m_wirelessShareEnabled ? tr("On") : tr("Off"));
    m_wirelessShare->setArtwork(m_wirelessShareEnabled ? "wireless-share" : "wireless-share-off");

    m_kdeConnect->setVisible(isMain);
    m_kdeConnect->setChecked(false);
    m_kdeConnect->setStatus(tr("Not Connected"));

    m_media->setVisible(isMain);
    m_media->setState(m_services.media());

    m_brightness->setVisible(isMain);
    m_brightness->slider()->setValue(m_services.brightness());

    const bool sink = m_services.hasSink();
    m_sound->setVisible(isMain && sink);
    m_sound->slider()->setValue(sink ? m_services.volume() : 0);
    m_sound->slider()->setIcon(panel::artwork(volumeIconName(sink, m_services.volume()), false));

    m_powerModeToggle->setVisible(isMain);
    m_nightShiftToggle->setVisible(isMain);
    m_captureToggle->setVisible(isMain);
    m_doNotDisturbToggle->setVisible(isMain);
    m_darkModeToggle->setVisible(isMain);
    m_keyLightToggle->setVisible(isMain);
    m_cameraToggle->setVisible(isMain);
    m_logoutToggle->setVisible(isMain);
    m_restartToggle->setVisible(isMain);
    m_powerOffToggle->setVisible(isMain);

    // Power Mode
    const std::string profile = m_services.powerProfile();
    if (profile == "performance") {
        m_powerModeToggle->setTitle(tr("Perf"));
        m_powerModeToggle->setArtwork("turbo");
        m_powerModeToggle->setActiveColor(ui::Color::rgb(0xFF9500));
        m_powerModeToggle->setChecked(true);
    } else if (profile == "power-saver") {
        m_powerModeToggle->setTitle(tr("Saver"));
        m_powerModeToggle->setArtwork("power-saver");
        m_powerModeToggle->setActiveColor(ui::Color::rgb(0x34C759));
        m_powerModeToggle->setChecked(true);
    } else {
        m_powerModeToggle->setTitle(tr("Balanced"));
        m_powerModeToggle->setArtwork("balanced");
        m_powerModeToggle->setChecked(false);
    }

    // Night Shift
    const bool nightLight = m_services.nightLightEnabled();
    m_nightShiftToggle->setChecked(nightLight);
    m_nightShiftToggle->setArtwork(nightLight ? "eye-protection" : "eye-protection-off");

    // Focus / DND
    const bool dnd = m_services.doNotDisturb();
    m_doNotDisturbToggle->setChecked(dnd);
    m_doNotDisturbToggle->setArtwork(dnd ? "do-not-disturb" : "do-not-disturb-off");

    // Dark Mode
    m_darkModeToggle->setChecked(dark);
    m_darkModeToggle->setTitle(dark ? tr("Dark") : tr("Light"));
    m_darkModeToggle->setArtwork(dark ? "dark-mode" : "light-mode");

    // Key Light
    const int keyLevel = m_services.keyboardBacklightLevel();
    if (keyLevel == 2) {
        m_keyLightToggle->setTitle(tr("Key High"));
        m_keyLightToggle->setArtwork("keyboard-backlight-high");
        m_keyLightToggle->setChecked(true);
    } else if (keyLevel == 1) {
        m_keyLightToggle->setTitle(tr("Key Low"));
        m_keyLightToggle->setArtwork("keyboard-backlight-low");
        m_keyLightToggle->setChecked(true);
    } else {
        m_keyLightToggle->setTitle(tr("Backlight"));
        m_keyLightToggle->setArtwork("keyboard-backlight-off");
        m_keyLightToggle->setChecked(false);
    }

    // Location
    m_locationToggle->setChecked(m_locationEnabled);
    m_locationToggle->setArtwork(m_locationEnabled ? "location-mode" : "location-mode-off");

    m_cameraToggle->setChecked(m_services.cameraEnabled());

    requestLayout();
    if (layoutHeight() != before && onHeightChanged)
        onHeightChanged();
}

double ControlCenterView::layoutHeight() const
{
    double h = 10.0; // top gap
    h += 32.0 + 10.0; // Header

    if (m_users->isVisible())
        h += m_users->heightForWidth(kWidth - 20) + 10.0;

    h += 2 * CardItem::kHeight + 10.0 + 10.0; // 2x2 Top Cards Grid
    h += MediaTile::kHeight + 10.0; // Media Tile
    h += 2 * 30.0 + 10.0 + 10.0; // Sliders
    h += 2 * CardItem::kHeight + 10.0 + 10.0; // 2x5 Quick Toggles
    return h;
}

double ControlCenterView::windowHeight(double scale) const
{
    double size = std::ceil(layoutHeight());
    for (int i = 0; i < 120 && std::abs(size * scale - std::round(size * scale)) > 0.001; ++i)
        ++size;
    return size;
}

void ControlCenterView::layout()
{
    if (m_darkModeShown != ui::Theme::current().darkMode) {
        m_darkModeShown = ui::Theme::current().darkMode;
        const bool dark = m_darkModeShown;
        m_darkModeToggle->setChecked(dark);
        m_darkModeToggle->setTitle(dark ? tr("Dark") : tr("Light"));
        m_darkModeToggle->setArtwork(dark ? "dark-mode" : "light-mode");
        m_media->setState(m_services.media());
        requestPaint();
    }

    const double contentWidth = kWidth - 2 * kGap; // 350
    double y = kGap;

    const bool isMain = m_subPanel == SubPanel::Main;
    if (!isMain) {
        m_backButton->setGeometry({16.0, y, 32.0, 32.0});
        m_title->setGeometry({56.0, y + 4.0, 250.0, m_title->sizeHint().height});
        y += 32.0 + kGap + 10.0;

        for (PanelRow *row : m_subRows) {
            if (!row->isVisible())
                continue;
            const double rowH = row->heightForWidth(contentWidth);
            row->setGeometry({kGap, y, contentWidth, rowH});
            y += rowH + 4.0;
        }
        return;
    }

    // Header
    m_backButton->setVisible(false);
    m_title->setGeometry({16.0, y + 4.0, 200.0, m_title->sizeHint().height});
    m_lockButton->setGeometry({contentWidth - 64.0, y, 32.0, 32.0});
    m_settingsButton->setGeometry({contentWidth - 22.0, y, 32.0, 32.0});
    y += 32.0 + kGap;

    // Device Users
    if (m_users->isVisible()) {
        const double h = m_users->heightForWidth(contentWidth);
        m_users->setGeometry({kGap, y, contentWidth, h});
        y += h + kGap;
    }

    // 2x2 Top Cards Grid
    const double cardWidth = (contentWidth - kGap) / 2.0;
    m_wireless->setGeometry({kGap, y, cardWidth, CardItem::kHeight});
    m_bluetooth->setGeometry({kGap + cardWidth + kGap, y, cardWidth, CardItem::kHeight});
    y += CardItem::kHeight + kGap;

    m_wirelessShare->setGeometry({kGap, y, cardWidth, CardItem::kHeight});
    m_kdeConnect->setGeometry({kGap + cardWidth + kGap, y, cardWidth, CardItem::kHeight});
    y += CardItem::kHeight + kGap;

    // Media Tile
    m_media->setGeometry({kGap, y, contentWidth, MediaTile::kHeight});
    y += MediaTile::kHeight + kGap;

    // Sliders
    if (m_brightness->isVisible()) {
        m_brightness->setGeometry({kGap, y, contentWidth, 30.0});
        y += 30.0 + kGap;
    }
    if (m_sound->isVisible()) {
        m_sound->setGeometry({kGap, y, contentWidth, 30.0});
        y += 30.0 + kGap;
    }

    // 2x5 Quick Toggles Grid
    const double toggleWidth = (contentWidth - 4 * 7.5) / 5.0;
    const double toggleGap = 7.5;

    std::vector<CardItem *> row1 = {m_powerModeToggle, m_nightShiftToggle, m_captureToggle, m_doNotDisturbToggle,
                                    m_darkModeToggle};
    for (size_t i = 0; i < row1.size(); ++i) {
        row1[i]->setGeometry({kGap + i * (toggleWidth + toggleGap), y, toggleWidth, CardItem::kHeight});
    }
    y += CardItem::kHeight + kGap;

    std::vector<CardItem *> row2 = {m_keyLightToggle, m_cameraToggle, m_logoutToggle, m_restartToggle,
                                    m_powerOffToggle};
    for (size_t i = 0; i < row2.size(); ++i) {
        row2[i]->setGeometry({kGap + i * (toggleWidth + toggleGap), y, toggleWidth, CardItem::kHeight});
    }
}

std::string volumeIconName(bool hasSink, double volume)
{
    const double level = hasSink ? volume * 100.0 : -1;
    if (level <= 0)
        return "audio-volume-muted-symbolic";
    if (level <= 25)
        return "audio-volume-low-symbolic";
    if (level <= 75)
        return "audio-volume-medium-symbolic";
    return "audio-volume-high-symbolic";
}

} // namespace cutefish::shell
