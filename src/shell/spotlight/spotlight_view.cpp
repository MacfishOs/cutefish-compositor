// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/spotlight/spotlight_view.h"

#include <algorithm>
#include <cmath>

#include "base/i18n.h"
#include "shell/common/app_launcher.h"
#include "shell/common/text_match.h"
#include "shell/controlcenter/panel_style.h"
#include "shell/controlcenter/tiles.h"
#include "shell/shell.h"
#include "shell/spotlight/spotlight.h"
#include "ui/icon_loader.h"
#include "ui/key_codes.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/text_field.h"
#include "ui/theme.h"

using cutefish::i18n::tr;

namespace cutefish::shell {

struct SettingsSearchItem {
    std::string module;
    std::string title;
    std::string description;
    std::vector<std::string> keywords;
    std::string icon;
};

static const std::vector<SettingsSearchItem> kSettingsModules = {
    {"display", "Display Settings", "Configure resolution, orientation, and brightness", {"display", "screen", "monitor", "resolution", "brightness"}, "preferences-desktop-display"},
    {"sound", "Sound Settings", "Configure audio output, volume, and input devices", {"sound", "audio", "volume", "speaker", "microphone"}, "preferences-desktop-sound"},
    {"wlan", "Wi-Fi & Networks", "Connect to wireless networks and manage connections", {"wifi", "network", "wireless", "internet", "wlan"}, "network-wireless-symbolic"},
    {"bluetooth", "Bluetooth Devices", "Pair and manage Bluetooth accessories", {"bluetooth", "devices", "pair", "wireless"}, "bluetooth-symbolic"},
    {"appearance", "Appearance & Wallpapers", "Toggle dark mode, themes, and change wallpapers", {"appearance", "theme", "dark", "light", "wallpaper", "accent"}, "preferences-desktop-theme"},
    {"power", "Power & Battery", "Manage power modes and battery behavior", {"power", "battery", "energy", "saver", "performance"}, "preferences-system-power-management"},
    {"keyboard", "Keyboard & Shortcuts", "Set keyboard layout and shortcut keys", {"keyboard", "input", "shortcuts", "layout"}, "preferences-desktop-keyboard"},
    {"mouse", "Mouse & Touchpad", "Configure pointer speed, scrolling, and gestures", {"mouse", "touchpad", "cursor", "pointer"}, "preferences-desktop-peripherals"},
    {"users", "Users & Accounts", "Manage user accounts, passwords, and avatars", {"users", "accounts", "password", "avatar", "profile"}, "preferences-system-users"},
    {"about", "About System", "View system version, processor, and hardware specs", {"about", "system", "info", "version", "specs"}, "preferences-system"}
};

// SpotlightResultRow
SpotlightResultRow::SpotlightResultRow()
{
    m_icon = addChild<SvgIcon>();
    m_title = addChild<ui::Label>();
    m_title->setFontSize(14.0);
    m_title->setWeight(500);
    m_title->setElide(ui::Label::Elide::Right);

    m_description = addChild<ui::Label>();
    m_description->setFontSize(11.0);
    m_description->setOpacity(0.65);
    m_description->setElide(ui::Label::Elide::Right);
}

void SpotlightResultRow::setData(const SpotlightSearchResult &result, bool selected)
{
    m_selected = selected;
    m_title->setText(result.title);
    m_description->setText(result.description);

    const bool dark = ui::Theme::current().darkMode;
    const bool whiteText = selected || dark;
    m_title->setColor(whiteText ? ui::Color::rgb(0xFFFFFF) : ui::Color::rgb(0x1C1C1E));
    m_description->setColor(whiteText ? panel::rgba(1, 1, 1, 0.75) : panel::rgba(0, 0, 0, 0.65));

    if (!result.icon.empty()) {
        if (result.icon.starts_with("/") || result.icon.ends_with(".svg"))
            m_icon->setFile(result.icon);
        else
            m_icon->setFile(panel::artwork(result.icon, dark));
    }
    requestLayout();
    requestPaint();
}

void SpotlightResultRow::layout()
{
    const double h = geometry().height;
    const double iconSize = 32.0;
    const double leftMargin = 16.0;

    m_icon->setGeometry({leftMargin, (h - iconSize) / 2.0, iconSize, iconSize});

    const double textX = leftMargin + iconSize + 14.0;
    const double textWidth = std::max(0.0, geometry().width - textX - 16.0);

    const double titleH = m_title->sizeHint().height;
    const double descH = m_description->sizeHint().height;
    const double top = (h - titleH - 2.0 - descH) / 2.0;

    m_title->setGeometry({textX, top, textWidth, titleH});
    m_description->setGeometry({textX, top + titleH + 2.0, textWidth, descH});
}

void SpotlightResultRow::paint(ui::Painter &painter)
{
    if (m_selected) {
        const ui::RectF rect = localRect().adjusted(8, 2, -8, -2);
        const ui::Color highlight = ui::Theme::current().highlight;
        painter.fillRoundedRect(rect, 10.0, highlight, ui::Corners::Continuous);
    }
}

void SpotlightResultRow::pointerPress(const ui::PointerEvent &event)
{
    if (event.button == ui::MouseButton::Left && onClicked)
        onClicked();
}

// SpotlightView
SpotlightView::SpotlightView(Shell *shell, Spotlight *spotlight)
    : m_shell(shell)
    , m_spotlight(spotlight)
{
    m_searchIcon = addChild<SvgIcon>();

    m_input = addChild<ui::TextField>();
    m_input->setFontSize(18.0);
    m_input->setPlaceholder(tr("Search applications, settings, and files..."));
    m_input->setFrameVisible(false);
    m_input->setFadesWhenDisabled(false);
    m_input->onTextChanged = [this] { performSearch(); };
    m_input->onAccepted = [this] { activateSelected(); };

    resetSearch();
}

SpotlightView::~SpotlightView() = default;

void SpotlightView::resetSearch()
{
    m_input->setText({});
    performSearch();
}

void SpotlightView::focusInput()
{
    if (m_input)
        m_input->setFocus(ui::FocusReason::Other);
}

double SpotlightView::desiredHeight() const
{
    if (m_results.empty())
        return kHeaderHeight;
    return kHeaderHeight + 1.0 + m_results.size() * kRowHeight + 10.0;
}

void SpotlightView::performSearch()
{
    const std::string query = m_input->text();
    m_results.clear();

    if (!query.empty()) {
        // 1. Applications
        if (m_shell) {
            for (const auto &entry : m_shell->applications().applications()) {
                if (!entry || entry->hidden || entry->noDisplay)
                    continue;
                bool match = containsIgnoringCase(entry->name, query)
                        || containsIgnoringCase(entry->genericName, query)
                        || containsIgnoringCase(entry->comment, query)
                        || containsIgnoringCase(entry->exec, query);
                if (!match) {
                    for (const auto &kw : entry->keywords) {
                        if (containsIgnoringCase(kw, query)) {
                            match = true;
                            break;
                        }
                    }
                }
                if (match) {
                    SpotlightSearchResult res;
                    res.kind = SpotlightSearchResult::Kind::Application;
                    res.title = !entry->name.empty() ? entry->name : entry->id;
                    res.description = !entry->genericName.empty() ? entry->genericName
                            : !entry->comment.empty() ? entry->comment : entry->exec;
                    res.icon = !entry->icon.empty() ? entry->icon : "application-x-executable";
                    res.entry = entry;
                    m_results.push_back(res);
                }
                if (m_results.size() >= kMaxResults)
                    break;
            }
        }

        // 2. Settings Modules
        if (m_results.size() < kMaxResults) {
            for (const auto &mod : kSettingsModules) {
                bool match = containsIgnoringCase(mod.title, query) || containsIgnoringCase(mod.description, query);
                if (!match) {
                    for (const auto &kw : mod.keywords) {
                        if (containsIgnoringCase(kw, query)) {
                            match = true;
                            break;
                        }
                    }
                }
                if (match) {
                    SpotlightSearchResult res;
                    res.kind = SpotlightSearchResult::Kind::Setting;
                    res.title = mod.title;
                    res.description = mod.description;
                    res.icon = mod.icon;
                    res.settingModule = mod.module;
                    m_results.push_back(res);
                }
                if (m_results.size() >= kMaxResults)
                    break;
            }
        }
    }

    // Update result row views
    while (m_rows.size() > m_results.size()) {
        takeChild(m_rows.back());
        m_rows.pop_back();
    }
    while (m_rows.size() < m_results.size()) {
        SpotlightResultRow *row = addChild<SpotlightResultRow>();
        m_rows.push_back(row);
    }

    m_selectedIndex = 0;
    updateSelection();
    requestLayout();
    requestPaint();
}

void SpotlightView::updateSelection()
{
    if (m_results.empty()) {
        m_selectedIndex = 0;
        return;
    }
    m_selectedIndex = std::clamp(m_selectedIndex, 0, int(m_results.size()) - 1);
    for (size_t i = 0; i < m_results.size(); ++i) {
        const bool sel = (int(i) == m_selectedIndex);
        m_rows[i]->setData(m_results[i], sel);
        m_rows[i]->onClicked = [this, i] {
            m_selectedIndex = int(i);
            activateSelected();
        };
    }
    requestPaint();
}

void SpotlightView::activateSelected()
{
    if (m_results.empty() || m_selectedIndex < 0 || m_selectedIndex >= int(m_results.size()))
        return;

    const SpotlightSearchResult &res = m_results[size_t(m_selectedIndex)];
    if (res.kind == SpotlightSearchResult::Kind::Application && res.entry && m_shell) {
        m_shell->launcher().launch(*res.entry);
    } else if (res.kind == SpotlightSearchResult::Kind::Setting && m_shell) {
        m_shell->openSettings({"-m", res.settingModule});
    }

    if (m_spotlight)
        m_spotlight->hide();
}

bool SpotlightView::keyPress(const ui::KeyEvent &event)
{
    if (event.key == ui::key::Down) {
        if (!m_results.empty()) {
            m_selectedIndex = (m_selectedIndex + 1) % int(m_results.size());
            updateSelection();
            return true;
        }
    } else if (event.key == ui::key::Up) {
        if (!m_results.empty()) {
            m_selectedIndex = (m_selectedIndex - 1 + int(m_results.size())) % int(m_results.size());
            updateSelection();
            return true;
        }
    } else if (event.key == ui::key::Escape) {
        if (m_spotlight)
            m_spotlight->hide();
        return true;
    }
    return ui::View::keyPress(event);
}

void SpotlightView::layout()
{
    const bool dark = ui::Theme::current().darkMode;
    m_searchIcon->setFile(panel::artwork("system-search-symbolic", dark));

    const double searchIconSize = 22.0;
    const double searchIconX = 18.0;
    const double searchIconY = (kHeaderHeight - searchIconSize) / 2.0;
    m_searchIcon->setGeometry({searchIconX, searchIconY, searchIconSize, searchIconSize});

    const double inputX = searchIconX + searchIconSize + 12.0;
    const double inputWidth = std::max(0.0, kWidth - inputX - 18.0);
    const double inputH = 36.0;
    m_input->setGeometry({inputX, (kHeaderHeight - inputH) / 2.0, inputWidth, inputH});

    double y = kHeaderHeight + 5.0;
    for (size_t i = 0; i < m_results.size(); ++i) {
        m_rows[i]->setGeometry({0, y, kWidth, kRowHeight});
        y += kRowHeight;
    }
}

void SpotlightView::paint(ui::Painter &painter)
{
    const bool dark = ui::Theme::current().darkMode;
    if (!m_results.empty()) {
        const double y = kHeaderHeight;
        const ui::Color line = dark ? panel::rgba(1, 1, 1, 0.12) : panel::rgba(0, 0, 0, 0.10);
        painter.fillRect({16, y, kWidth - 32, 1}, line);
    }
}

} // namespace cutefish::shell
