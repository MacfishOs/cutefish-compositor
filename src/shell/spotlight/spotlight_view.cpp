// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/spotlight/spotlight_view.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "base/i18n.h"
#include "base/process.h"
#include "shell/common/app_launcher.h"
#include "shell/common/text_match.h"
#include "shell/controlcenter/panel_style.h"
#include "shell/controlcenter/tiles.h"
#include "shell/shell.h"
#include "shell/spotlight/spotlight.h"
#include "shell/spotlight/spotlight_evaluator.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/window.h"

using cutefish::i18n::tr;
namespace fs = std::filesystem;

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

static std::string trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

static std::string formatFileSize(uintmax_t bytes)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        double gb = bytes / (1024.0 * 1024.0 * 1024.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
        return buf;
    }
    if (bytes >= 1024ULL * 1024ULL) {
        double mb = bytes / (1024.0 * 1024.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
        return buf;
    }
    if (bytes >= 1024) {
        double kb = bytes / 1024.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f KB", kb);
        return buf;
    }
    return std::to_string(bytes) + " B";
}

static std::string iconForExtension(const std::string &ext)
{
    if (ext == ".pdf") return "application-pdf";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".svg" || ext == ".gif" || ext == ".webp")
        return "image-x-generic";
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".m4a")
        return "audio-x-generic";
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".webm")
        return "video-x-generic";
    if (ext == ".zip" || ext == ".tar" || ext == ".gz" || ext == ".xz" || ext == ".7z")
        return "package-x-generic";
    if (ext == ".txt" || ext == ".md" || ext == ".json" || ext == ".csv" || ext == ".xml" ||
        ext == ".cpp" || ext == ".h" || ext == ".py" || ext == ".sh")
        return "text-x-generic";
    return "text-x-generic";
}

// ---------------------------------------------------------------------------
// SpotlightClearButton
// ---------------------------------------------------------------------------
SpotlightClearButton::SpotlightClearButton() = default;

void SpotlightClearButton::paint(ui::Painter &painter)
{
    const double size = geometry().width;
    const ui::RectF rect(0, 0, size, size);
    const bool dark = ui::Theme::current().darkMode;

    ui::Color circleColor = dark ? panel::rgba(1, 1, 1, m_hovered ? 0.25 : 0.16)
                                 : panel::rgba(0, 0, 0, m_hovered ? 0.20 : 0.12);
    painter.fillRoundedRect(rect, size / 2.0, circleColor);

    const ui::Color crossColor = dark ? panel::rgba(1, 1, 1, 0.85) : panel::rgba(0, 0, 0, 0.70);
    const double pad = size * 0.32;
    painter.drawLine({pad, pad}, {size - pad, size - pad}, crossColor, 1.4);
    painter.drawLine({size - pad, pad}, {pad, size - pad}, crossColor, 1.4);
}

void SpotlightClearButton::pointerEnter()
{
    m_hovered = true;
    requestPaint();
}

void SpotlightClearButton::pointerLeave()
{
    m_hovered = false;
    requestPaint();
}

void SpotlightClearButton::pointerPress(const ui::PointerEvent &event)
{
    if (event.button == ui::MouseButton::Left && onClicked)
        onClicked();
}

// ---------------------------------------------------------------------------
// SpotlightSectionHeader
// ---------------------------------------------------------------------------
SpotlightSectionHeader::SpotlightSectionHeader()
{
    m_label = addChild<ui::Label>();
    m_label->setFontSize(10.5);
    m_label->setWeight(600);
}

void SpotlightSectionHeader::setTitle(const std::string &title)
{
    m_label->setText(title);
    const bool dark = ui::Theme::current().darkMode;
    m_label->setColor(dark ? panel::rgba(1, 1, 1, 0.45) : panel::rgba(0, 0, 0, 0.45));
    requestLayout();
    requestPaint();
}

void SpotlightSectionHeader::layout()
{
    m_label->setGeometry({18.0, 4.0, std::max(0.0, geometry().width - 36.0), 16.0});
}

void SpotlightSectionHeader::paint(ui::Painter &) {}

// ---------------------------------------------------------------------------
// SpotlightResultRow
// ---------------------------------------------------------------------------
SpotlightResultRow::SpotlightResultRow()
{
    m_icon = addChild<ui::ThemeIcon>(std::string(), 28.0);
    m_title = addChild<ui::Label>();
    m_title->setFontSize(13.5);
    m_title->setWeight(500);
    m_title->setElide(ui::Label::Elide::Right);

    m_subtitle = addChild<ui::Label>();
    m_subtitle->setFontSize(11.0);
    m_subtitle->setOpacity(0.65);
    m_subtitle->setElide(ui::Label::Elide::Right);

    m_badge = addChild<ui::Label>();
    m_badge->setFontSize(10.5);
    m_badge->setWeight(500);
    m_badge->setOpacity(0.55);
}

void SpotlightResultRow::setData(const SpotlightSearchResult &result, bool selected)
{
    m_selected = selected;
    m_title->setText(result.title);
    m_subtitle->setText(result.subtitle);
    m_badge->setText(result.badge);

    const bool dark = ui::Theme::current().darkMode;
    const bool whiteText = selected || dark;

    m_title->setColor(whiteText ? ui::Color::rgb(0xFFFFFF) : ui::Color::rgb(0x1C1C1E));
    m_subtitle->setColor(whiteText ? panel::rgba(1, 1, 1, 0.70) : panel::rgba(0, 0, 0, 0.60));
    m_badge->setColor(selected ? ui::Color::rgb(0xFFFFFF)
                               : (dark ? panel::rgba(1, 1, 1, 0.45) : panel::rgba(0, 0, 0, 0.45)));

    if (!result.icon.empty())
        m_icon->setIconName(result.icon);

    requestLayout();
    requestPaint();
}

void SpotlightResultRow::layout()
{
    const double h = geometry().height;
    const double w = geometry().width;
    const double iconSize = 28.0;
    const double leftMargin = 16.0;

    m_icon->setGeometry({leftMargin, (h - iconSize) / 2.0, iconSize, iconSize});

    const double badgeWidth = m_badge->sizeHint().width;
    const double rightPadding = 16.0;
    if (badgeWidth > 0) {
        m_badge->setGeometry({w - badgeWidth - rightPadding, (h - 16.0) / 2.0, badgeWidth, 16.0});
    }

    const double textX = leftMargin + iconSize + 12.0;
    const double maxTextWidth = std::max(0.0, w - textX - badgeWidth - rightPadding - 8.0);

    const double titleH = m_title->sizeHint().height;
    const double subH = m_subtitle->sizeHint().height;
    const double totalTextH = titleH + 1.0 + subH;
    const double top = (h - totalTextH) / 2.0;

    m_title->setGeometry({textX, top, maxTextWidth, titleH});
    m_subtitle->setGeometry({textX, top + titleH + 1.0, maxTextWidth, subH});
}

void SpotlightResultRow::paint(ui::Painter &painter)
{
    if (m_selected) {
        const ui::RectF rect = localRect().adjusted(10, 2, -10, -2);
        const ui::Color highlight = ui::Theme::current().highlight;
        painter.fillRoundedRect(rect, 8.0, highlight, ui::Corners::Continuous);
    }
}

void SpotlightResultRow::pointerEnter()
{
    if (onHovered)
        onHovered();
}

void SpotlightResultRow::pointerPress(const ui::PointerEvent &event)
{
    if (event.button == ui::MouseButton::Left && onClicked)
        onClicked();
}

// ---------------------------------------------------------------------------
// SpotlightPreviewPane
// ---------------------------------------------------------------------------
SpotlightPreviewPane::SpotlightPreviewPane()
{
    m_largeIcon = addChild<ui::ThemeIcon>(std::string(), 60.0);

    m_title = addChild<ui::Label>();
    m_title->setFontSize(15.0);
    m_title->setWeight(600);
    m_title->setElide(ui::Label::Elide::Right);

    m_category = addChild<ui::Label>();
    m_category->setFontSize(11.0);
    m_category->setWeight(500);
    m_category->setOpacity(0.55);
    m_category->setElide(ui::Label::Elide::Right);

    m_primaryDetail = addChild<ui::Label>();
    m_primaryDetail->setFontSize(12.0);
    m_primaryDetail->setElide(ui::Label::Elide::Right);

    m_secondaryDetail = addChild<ui::Label>();
    m_secondaryDetail->setFontSize(11.0);
    m_secondaryDetail->setOpacity(0.60);
    m_secondaryDetail->setElide(ui::Label::Elide::Right);

    m_hintBadge = addChild<ui::Label>();
    m_hintBadge->setFontSize(11.0);
    m_hintBadge->setWeight(500);
}

void SpotlightPreviewPane::setData(const SpotlightSearchResult *result)
{
    m_hasData = (result != nullptr);
    if (!result) {
        requestPaint();
        return;
    }

    m_currentCategory = result->category;
    m_title->setText(result->title);
    m_category->setText(result->badge);

    if (result->category == SpotlightCategory::Calculator) {
        m_primaryDetail->setText(result->details);
        m_primaryDetail->setFontSize(22.0);
        m_primaryDetail->setWeight(700);
        m_secondaryDetail->setText("Calculation • " + result->subtitle);
    } else {
        m_primaryDetail->setText(result->details.empty() ? result->subtitle : result->details);
        m_primaryDetail->setFontSize(12.0);
        m_primaryDetail->setWeight(400);
        m_secondaryDetail->setText(result->actionPayload.empty() ? result->subtitle : result->actionPayload);
    }

    m_hintBadge->setText(result->actionHint.empty() ? "↩ Open" : "↩ " + result->actionHint);

    const bool dark = ui::Theme::current().darkMode;
    m_title->setColor(dark ? ui::Color::rgb(0xFFFFFF) : ui::Color::rgb(0x1C1C1E));
    m_category->setColor(dark ? panel::rgba(1, 1, 1, 0.50) : panel::rgba(0, 0, 0, 0.50));
    m_primaryDetail->setColor(dark ? panel::rgba(1, 1, 1, 0.90) : panel::rgba(0, 0, 0, 0.85));
    m_secondaryDetail->setColor(dark ? panel::rgba(1, 1, 1, 0.55) : panel::rgba(0, 0, 0, 0.55));
    m_hintBadge->setColor(dark ? panel::rgba(1, 1, 1, 0.75) : panel::rgba(0, 0, 0, 0.70));

    if (!result->icon.empty())
        m_largeIcon->setIconName(result->icon);

    requestLayout();
    requestPaint();
}

void SpotlightPreviewPane::layout()
{
    if (!m_hasData)
        return;

    const double w = geometry().width;
    const double h = geometry().height;

    // Centered big icon
    const double iconSize = 60.0;
    m_largeIcon->setGeometry({(w - iconSize) / 2.0, 24.0, iconSize, iconSize});

    // Centered title
    const double titleH = m_title->sizeHint().height;
    m_title->setGeometry({16.0, 94.0, w - 32.0, titleH});

    // Centered category
    const double catH = m_category->sizeHint().height;
    m_category->setGeometry({16.0, 94.0 + titleH + 2.0, w - 32.0, catH});

    // Details below divider
    const double detTop = 146.0;
    const double primH = m_primaryDetail->sizeHint().height;
    m_primaryDetail->setGeometry({18.0, detTop, w - 36.0, primH});

    const double secH = m_secondaryDetail->sizeHint().height;
    m_secondaryDetail->setGeometry({18.0, detTop + primH + 4.0, w - 36.0, secH});

    // Hint badge at bottom
    const double hintW = m_hintBadge->sizeHint().width;
    const double hintH = 24.0;
    const double hintPillW = hintW + 20.0;
    m_hintBadge->setGeometry({(w - hintW) / 2.0, h - hintH - 12.0, hintW, hintH});
}

void SpotlightPreviewPane::paint(ui::Painter &painter)
{
    if (!m_hasData)
        return;

    const double w = geometry().width;
    const double h = geometry().height;
    const bool dark = ui::Theme::current().darkMode;

    // Subtle horizontal divider in preview pane
    const ui::Color dividerColor = dark ? panel::rgba(1, 1, 1, 0.08) : panel::rgba(0, 0, 0, 0.06);
    painter.fillRect({20.0, 134.0, w - 40.0, 1.0}, dividerColor);

    // Bottom action hint pill plate
    const double hintW = m_hintBadge->sizeHint().width;
    const double hintH = 24.0;
    const double hintPillW = hintW + 20.0;
    const ui::RectF pillRect((w - hintPillW) / 2.0, h - hintH - 12.0, hintPillW, hintH);
    const ui::Color pillPlate = dark ? panel::rgba(1, 1, 1, 0.10) : panel::rgba(0, 0, 0, 0.06);
    painter.fillRoundedRect(pillRect, hintH / 2.0, pillPlate);
}

// ---------------------------------------------------------------------------
// SpotlightView
// ---------------------------------------------------------------------------
SpotlightView::SpotlightView(Shell *shell, Spotlight *spotlight)
    : m_shell(shell)
    , m_spotlight(spotlight)
{
    m_searchIcon = addChild<SvgIcon>();

    m_input = addChild<ui::TextField>();
    m_input->setFontSize(19.0);
    m_input->setPlaceholder(tr("Spotlight Search"));
    m_input->setFrameVisible(false);
    m_input->setFadesWhenDisabled(false);
    m_input->onTextChanged = [this] { performSearch(); };
    m_input->onAccepted = [this] { activateSelected(); };

    m_clearButton = addChild<SpotlightClearButton>();
    m_clearButton->onClicked = [this] {
        resetSearch();
        focusInput();
    };

    m_previewPane = addChild<SpotlightPreviewPane>();

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

    double listHeight = 8.0;
    SpotlightCategory lastCat = static_cast<SpotlightCategory>(-1);
    for (const auto &res : m_results) {
        if (res.category != lastCat) {
            listHeight += kSectionHeight;
            lastCat = res.category;
        }
        listHeight += kRowHeight;
    }
    listHeight += 10.0;

    const double minPreviewHeight = 280.0;
    const double contentHeight = std::max(listHeight, minPreviewHeight);
    return std::min(520.0, kHeaderHeight + contentHeight);
}

void SpotlightView::performSearch()
{
    const std::string query = m_input->text();
    m_results.clear();

    if (!query.empty()) {
        const std::string trimmed = trim(query);
        const std::string lowerTrimmed = toLower(trimmed);

        // 1. Instant Calculator & Conversions
        if (auto mathRes = SpotlightEvaluator::evaluate(trimmed)) {
            SpotlightSearchResult calc;
            calc.category = SpotlightCategory::Calculator;
            calc.title = mathRes->formatted;
            calc.subtitle = mathRes->isConversion ? "Unit Conversion" : "Calculation • " + mathRes->expression;
            calc.details = mathRes->result;
            calc.icon = "accessories-calculator";
            calc.actionPayload = mathRes->result;
            calc.badge = "Calculation";
            calc.actionHint = "Copy Result";
            m_results.push_back(calc);
        }

        // 2. Applications & Top Hit
        if (m_shell) {
            std::vector<std::pair<int, DesktopEntryPtr>> scoredApps;
            for (const auto &entry : m_shell->applications().applications()) {
                if (!entry || entry->hidden || entry->noDisplay)
                    continue;

                std::string lowerName = toLower(entry->name);
                int score = 0;
                if (lowerName == lowerTrimmed) {
                    score = 100;
                } else if (lowerName.starts_with(lowerTrimmed)) {
                    score = 80;
                } else if (containsIgnoringCase(entry->name, trimmed)) {
                    score = 60;
                } else if (containsIgnoringCase(entry->genericName, trimmed)) {
                    score = 40;
                } else if (containsIgnoringCase(entry->comment, trimmed)) {
                    score = 30;
                } else {
                    for (const auto &kw : entry->keywords) {
                        if (containsIgnoringCase(kw, trimmed)) {
                            score = 25;
                            break;
                        }
                    }
                }

                if (score > 0)
                    scoredApps.emplace_back(score, entry);
            }

            std::stable_sort(scoredApps.begin(), scoredApps.end(), [](const auto &a, const auto &b) {
                return a.first > b.first;
            });

            bool hasTopHit = false;
            for (size_t i = 0; i < scoredApps.size() && i < 5; ++i) {
                const auto &entry = scoredApps[i].second;
                SpotlightSearchResult app;
                if (i == 0 && scoredApps[0].first >= 80 && m_results.empty()) {
                    app.category = SpotlightCategory::TopHit;
                    app.badge = "TOP HIT";
                    hasTopHit = true;
                } else {
                    app.category = SpotlightCategory::Applications;
                    app.badge = "Application";
                }
                app.title = !entry->name.empty() ? entry->name : entry->id;
                app.subtitle = !entry->genericName.empty() ? entry->genericName
                             : !entry->comment.empty() ? entry->comment : entry->exec;
                app.details = !entry->comment.empty() ? entry->comment : entry->genericName;
                app.icon = !entry->icon.empty() ? entry->icon : "application-x-executable";
                app.actionPayload = entry->exec;
                app.entry = entry;
                app.actionHint = "Open Application";
                m_results.push_back(app);
            }
        }

        // 3. System Settings
        for (const auto &mod : kSettingsModules) {
            bool match = containsIgnoringCase(mod.title, trimmed) || containsIgnoringCase(mod.description, trimmed);
            if (!match) {
                for (const auto &kw : mod.keywords) {
                    if (containsIgnoringCase(kw, trimmed)) {
                        match = true;
                        break;
                    }
                }
            }
            if (match) {
                SpotlightSearchResult st;
                st.category = SpotlightCategory::Settings;
                st.title = mod.title;
                st.subtitle = mod.description;
                st.details = mod.description;
                st.icon = mod.icon;
                st.settingModule = mod.module;
                st.badge = "Setting";
                st.actionHint = "Open Settings";
                m_results.push_back(st);
                if (m_results.size() >= 8) break;
            }
        }

        // 4. System Actions
        struct SystemActionItem {
            std::string keyword;
            std::string title;
            std::string description;
            std::string icon;
            std::string payload;
        };
        static const std::vector<SystemActionItem> kActions = {
            {"lock", "Lock Screen", "Lock your computer session", "system-lock-screen", "lock"},
            {"logout", "Log Out", "Log out of Cutefish session", "system-log-out", "logout"},
            {"sleep", "Sleep", "Put your computer to sleep", "system-suspend", "sleep"},
            {"restart", "Restart…", "Reboot the operating system", "system-reboot", "restart"},
            {"shutdown", "Shut Down…", "Power off the computer", "system-shutdown", "shutdown"},
            {"terminal", "Terminal", "Open Cutefish terminal emulator", "utilities-terminal", "terminal"},
            {"screenshot", "Take Screenshot", "Capture screen or window", "cutefish-screenshot", "screenshot"},
            {"trash", "Trash", "Open Trash bin", "user-trash", "trash"}
        };
        for (const auto &act : kActions) {
            if (containsIgnoringCase(act.keyword, trimmed) || containsIgnoringCase(act.title, trimmed)) {
                SpotlightSearchResult actionRes;
                actionRes.category = SpotlightCategory::SystemActions;
                actionRes.title = act.title;
                actionRes.subtitle = act.description;
                actionRes.details = act.description;
                actionRes.icon = act.icon;
                actionRes.actionPayload = act.payload;
                actionRes.badge = "Action";
                actionRes.actionHint = "Execute Action";
                m_results.push_back(actionRes);
            }
        }

        // 5. Files & Documents Search
        if (trimmed.length() >= 2 && m_results.size() < 7) {
            const char *home = std::getenv("HOME");
            if (home) {
                const std::vector<std::string> searchDirs = {
                    std::string(home) + "/Desktop",
                    std::string(home) + "/Documents",
                    std::string(home) + "/Downloads",
                    std::string(home) + "/Pictures",
                    std::string(home) + "/Music",
                    std::string(home) + "/Videos"
                };

                int filesFound = 0;
                for (const auto &dirPath : searchDirs) {
                    if (filesFound >= 3 || m_results.size() >= 8)
                        break;
                    std::error_code ec;
                    if (!fs::exists(dirPath, ec) || !fs::is_directory(dirPath, ec))
                        continue;

                    for (auto it = fs::recursive_directory_iterator(dirPath, fs::directory_options::skip_permission_denied, ec);
                         it != fs::recursive_directory_iterator(); ++it) {
                        if (ec) break;
                        if (it.depth() > 2) {
                            it.pop();
                            continue;
                        }

                        const auto &entry = *it;
                        const std::string filename = entry.path().filename().string();
                        if (filename.starts_with(".")) continue;

                        if (containsIgnoringCase(filename, trimmed)) {
                            SpotlightSearchResult fileRes;
                            fileRes.category = SpotlightCategory::Documents;
                            fileRes.title = filename;

                            uintmax_t size = entry.is_regular_file(ec) ? entry.file_size(ec) : 0;
                            std::string sizeStr = formatFileSize(size);
                            fileRes.subtitle = entry.path().parent_path().string() + " • " + sizeStr;
                            fileRes.details = "Location: " + entry.path().string() + "\nSize: " + sizeStr;
                            fileRes.icon = iconForExtension(entry.path().extension().string());
                            fileRes.actionPayload = entry.path().string();
                            fileRes.badge = "Document";
                            fileRes.actionHint = "Open File";
                            m_results.push_back(fileRes);

                            if (++filesFound >= 3 || m_results.size() >= 8)
                                break;
                        }
                    }
                }
            }
        }

        // 6. Web Search & URL Navigation
        bool isUrl = (trimmed.starts_with("http://") || trimmed.starts_with("https://") ||
                      (trimmed.find(' ') == std::string::npos && (trimmed.find(".com") != std::string::npos ||
                                                                  trimmed.find(".org") != std::string::npos ||
                                                                  trimmed.find(".net") != std::string::npos ||
                                                                  trimmed.find(".io") != std::string::npos ||
                                                                  trimmed.find(".dev") != std::string::npos)));
        if (isUrl) {
            std::string url = trimmed;
            if (!url.starts_with("http://") && !url.starts_with("https://"))
                url = "https://" + url;
            SpotlightSearchResult urlRes;
            urlRes.category = SpotlightCategory::Web;
            urlRes.title = "Open " + url;
            urlRes.subtitle = "Navigate to website";
            urlRes.details = url;
            urlRes.icon = "internet-web-browser";
            urlRes.actionPayload = url;
            urlRes.badge = "Web";
            urlRes.actionHint = "Open in Browser";
            m_results.push_back(urlRes);
        } else {
            SpotlightSearchResult webRes;
            webRes.category = SpotlightCategory::Web;
            webRes.title = "Search Google for \"" + trimmed + "\"";
            webRes.subtitle = "Search the web in default browser";
            webRes.details = "Google Search: " + trimmed;
            webRes.icon = "preferences-system-network";
            webRes.actionPayload = "https://www.google.com/search?q=" + trimmed;
            webRes.badge = "Google";
            webRes.actionHint = "Search Web";
            m_results.push_back(webRes);
        }
    }

    // Clear old list items
    for (auto *item : m_listItems)
        takeChild(item);
    m_listItems.clear();
    m_rows.clear();
    m_headers.clear();

    // Rebuild list items (headers + rows)
    SpotlightCategory lastCat = static_cast<SpotlightCategory>(-1);
    for (size_t i = 0; i < m_results.size(); ++i) {
        const auto &res = m_results[i];
        if (res.category != lastCat) {
            std::string headerTitle;
            switch (res.category) {
            case SpotlightCategory::TopHit: headerTitle = "TOP HIT"; break;
            case SpotlightCategory::Calculator: headerTitle = "CALCULATOR"; break;
            case SpotlightCategory::Applications: headerTitle = "APPLICATIONS"; break;
            case SpotlightCategory::Settings: headerTitle = "SYSTEM SETTINGS"; break;
            case SpotlightCategory::Documents: headerTitle = "DOCUMENTS"; break;
            case SpotlightCategory::SystemActions: headerTitle = "SYSTEM ACTIONS"; break;
            case SpotlightCategory::Web: headerTitle = "SEARCH THE WEB"; break;
            }
            SpotlightSectionHeader *hdr = addChild<SpotlightSectionHeader>();
            hdr->setTitle(headerTitle);
            m_listItems.push_back(hdr);
            m_headers.push_back(hdr);
            lastCat = res.category;
        }

        SpotlightResultRow *row = addChild<SpotlightResultRow>();
        m_listItems.push_back(row);
        m_rows.push_back(row);
    }

    m_selectedIndex = 0;
    m_scrollAccumulator = 0.0;
    updateSelection();

    if (onHeightChanged)
        onHeightChanged();
    requestLayout();
    requestPaint();
}

void SpotlightView::updateSelection()
{
    if (m_results.empty()) {
        m_selectedIndex = 0;
        m_previewPane->setData(nullptr);
        return;
    }

    m_selectedIndex = std::clamp(m_selectedIndex, 0, int(m_results.size()) - 1);
    for (size_t i = 0; i < m_results.size(); ++i) {
        const bool sel = (int(i) == m_selectedIndex);
        m_rows[i]->setData(m_results[i], sel);
        m_rows[i]->onHovered = [this, i] {
            if (m_selectedIndex != int(i)) {
                m_selectedIndex = int(i);
                updateSelection();
            }
        };
        m_rows[i]->onClicked = [this, i] {
            m_selectedIndex = int(i);
            activateSelected();
        };
    }

    m_previewPane->setData(&m_results[size_t(m_selectedIndex)]);
    requestPaint();
}

void SpotlightView::activateSelected()
{
    if (m_results.empty() || m_selectedIndex < 0 || m_selectedIndex >= int(m_results.size()))
        return;

    const SpotlightSearchResult &res = m_results[size_t(m_selectedIndex)];
    switch (res.category) {
    case SpotlightCategory::Calculator:
        base::Process::startDetached("wl-copy", {res.actionPayload});
        break;
    case SpotlightCategory::TopHit:
        if (res.entry && m_shell)
            m_shell->launcher().launch(*res.entry);
        else if (!res.settingModule.empty() && m_shell)
            m_shell->openSettings({"-m", res.settingModule});
        break;
    case SpotlightCategory::Applications:
        if (res.entry && m_shell)
            m_shell->launcher().launch(*res.entry);
        break;
    case SpotlightCategory::Settings:
        if (m_shell)
            m_shell->openSettings({"-m", res.settingModule});
        break;
    case SpotlightCategory::Documents:
        base::Process::startDetached("xdg-open", {res.actionPayload});
        break;
    case SpotlightCategory::SystemActions:
        if (res.actionPayload == "lock")
            base::Process::startDetached("loginctl", {"lock-session"});
        else if (res.actionPayload == "logout")
            base::Process::startDetached("cutefish-session-logout", {});
        else if (res.actionPayload == "sleep")
            base::Process::startDetached("systemctl", {"suspend"});
        else if (res.actionPayload == "restart")
            base::Process::startDetached("systemctl", {"reboot"});
        else if (res.actionPayload == "shutdown")
            base::Process::startDetached("systemctl", {"poweroff"});
        else if (res.actionPayload == "terminal")
            base::Process::startDetached("cutefish-terminal", {});
        else if (res.actionPayload == "screenshot")
            base::Process::startDetached("cutefish-screenshot", {});
        else if (res.actionPayload == "trash")
            base::Process::startDetached("xdg-open", {"trash:///"});
        break;
    case SpotlightCategory::Web:
        base::Process::startDetached("xdg-open", {res.actionPayload});
        break;
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

bool SpotlightView::scroll(const ui::ScrollEvent &event)
{
    if (m_results.size() <= 1)
        return false;

    double delta = event.deltaY;
    if (event.value120Y != 0)
        delta = (event.value120Y / 120.0) * 20.0;

    m_scrollAccumulator += delta;
    const double kThreshold = 14.0;

    if (std::abs(m_scrollAccumulator) >= kThreshold) {
        int steps = static_cast<int>(m_scrollAccumulator / kThreshold);
        m_scrollAccumulator -= steps * kThreshold;
        int newIndex = std::clamp(m_selectedIndex + steps, 0, int(m_results.size()) - 1);
        if (newIndex != m_selectedIndex) {
            m_selectedIndex = newIndex;
            updateSelection();
        }
    }
    return true;
}

void SpotlightView::layout()
{
    const bool dark = ui::Theme::current().darkMode;
    m_searchIcon->setFile(panel::artwork("system-search-symbolic", dark));

    // Header layout
    const double searchIconSize = 20.0;
    const double searchIconX = 20.0;
    const double searchIconY = (kHeaderHeight - searchIconSize) / 2.0;
    m_searchIcon->setGeometry({searchIconX, searchIconY, searchIconSize, searchIconSize});

    const bool hasText = !m_input->text().empty();
    m_clearButton->setVisible(hasText);

    const double clearSize = 20.0;
    const double clearX = kWidth - 20.0 - clearSize;
    const double clearY = (kHeaderHeight - clearSize) / 2.0;
    m_clearButton->setGeometry({clearX, clearY, clearSize, clearSize});

    const double inputX = searchIconX + searchIconSize + 14.0;
    const double inputRightMargin = hasText ? (kWidth - clearX + 8.0) : 20.0;
    const double inputWidth = std::max(0.0, kWidth - inputX - inputRightMargin);
    const double inputH = 38.0;
    m_input->setGeometry({inputX, (kHeaderHeight - inputH) / 2.0, inputWidth, inputH});

    // Content layout (Left list pane & Right preview pane)
    if (!m_results.empty()) {
        double y = kHeaderHeight + 6.0;
        size_t rowIndex = 0;
        size_t headerIndex = 0;

        for (auto *item : m_listItems) {
            if (headerIndex < m_headers.size() && item == m_headers[headerIndex]) {
                item->setGeometry({0, y, kLeftPaneWidth, kSectionHeight});
                y += kSectionHeight;
                ++headerIndex;
            } else if (rowIndex < m_rows.size() && item == m_rows[rowIndex]) {
                item->setGeometry({0, y, kLeftPaneWidth, kRowHeight});
                y += kRowHeight;
                ++rowIndex;
            }
        }

        const double contentH = std::max(y - kHeaderHeight, 280.0);
        m_previewPane->setVisible(true);
        m_previewPane->setGeometry({kLeftPaneWidth, kHeaderHeight + 2.0, kRightPaneWidth, contentH});
    } else {
        m_previewPane->setVisible(false);
    }
}

void SpotlightView::paint(ui::Painter &painter)
{
    const bool dark = ui::Theme::current().darkMode;
    if (!m_results.empty()) {
        // Horizontal divider below header
        const ui::Color line = dark ? panel::rgba(1, 1, 1, 0.12) : panel::rgba(0, 0, 0, 0.10);
        painter.fillRect({16, kHeaderHeight, kWidth - 32, 1}, line);

        // Vertical divider between left list and right preview
        const double contentH = geometry().height - kHeaderHeight - 8.0;
        painter.fillRect({kLeftPaneWidth, kHeaderHeight + 6.0, 1.0, contentH}, line);
    }
}

} // namespace cutefish::shell
