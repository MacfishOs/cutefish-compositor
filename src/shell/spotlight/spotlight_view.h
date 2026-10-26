// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_VIEW_H_
#define CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_VIEW_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/signal.h"
#include "shell/common/applications.h"
#include "ui/view.h"

namespace cutefish::ui {
class Label;
class TextField;
class ThemeIcon;
} // namespace cutefish::ui

namespace cutefish::shell {

class Shell;
class Spotlight;
class SvgIcon;

enum class SpotlightCategory {
    TopHit,
    Calculator,
    Applications,
    Settings,
    Documents,
    SystemActions,
    Web
};

struct SpotlightSearchResult {
    SpotlightCategory category = SpotlightCategory::Applications;
    std::string title;
    std::string subtitle;
    std::string details;
    std::string icon;
    std::string actionPayload;
    DesktopEntryPtr entry;
    std::string settingModule;
    std::string badge;
    std::string actionHint;
};

// Clear button inside search field
class SpotlightClearButton final : public ui::View
{
public:
    SpotlightClearButton();
    bool acceptsPointer() const override { return true; }
    std::function<void()> onClicked;

protected:
    void paint(ui::Painter &painter) override;
    void pointerEnter() override;
    void pointerLeave() override;
    void pointerPress(const ui::PointerEvent &event) override;

private:
    bool m_hovered = false;
};

// Section Header row (e.g. "TOP HIT", "APPLICATIONS", "SYSTEM SETTINGS", etc.)
class SpotlightSectionHeader final : public ui::View
{
public:
    SpotlightSectionHeader();
    void setTitle(const std::string &title);

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;

private:
    ui::Label *m_label = nullptr;
};

// Result row on left list pane
class SpotlightResultRow final : public ui::View
{
public:
    SpotlightResultRow();

    bool acceptsPointer() const override { return true; }
    void setData(const SpotlightSearchResult &result, bool selected);

    std::function<void()> onClicked;
    std::function<void()> onHovered;

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;
    void pointerEnter() override;
    void pointerPress(const ui::PointerEvent &event) override;

private:
    ui::ThemeIcon *m_icon = nullptr;
    ui::Label *m_title = nullptr;
    ui::Label *m_subtitle = nullptr;
    ui::Label *m_badge = nullptr;
    bool m_selected = false;
};

// Rich Preview Pane on right side
class SpotlightPreviewPane final : public ui::View
{
public:
    SpotlightPreviewPane();
    void setData(const SpotlightSearchResult *result);

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;

private:
    ui::ThemeIcon *m_largeIcon = nullptr;
    ui::Label *m_title = nullptr;
    ui::Label *m_category = nullptr;
    ui::Label *m_primaryDetail = nullptr;
    ui::Label *m_secondaryDetail = nullptr;
    ui::Label *m_hintBadge = nullptr;
    bool m_hasData = false;
    SpotlightCategory m_currentCategory = SpotlightCategory::Applications;
};

// Main Spotlight Floating View
class SpotlightView final : public ui::View
{
public:
    static constexpr double kWidth = 700.0;
    static constexpr double kHeaderHeight = 60.0;
    static constexpr double kRowHeight = 46.0;
    static constexpr double kSectionHeight = 24.0;
    static constexpr double kLeftPaneWidth = 410.0;
    static constexpr double kRightPaneWidth = 290.0;

    SpotlightView(Shell *shell, Spotlight *spotlight);
    ~SpotlightView() override;

    void resetSearch();
    void focusInput();

    double desiredHeight() const;

    std::function<void()> onHeightChanged;
    base::Signal<> dismissRequested;

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;
    bool keyPress(const ui::KeyEvent &event) override;
    bool scroll(const ui::ScrollEvent &event) override;

private:
    void performSearch();
    void updateSelection();
    void activateSelected();

    Shell *m_shell = nullptr;
    Spotlight *m_spotlight = nullptr;
    SvgIcon *m_searchIcon = nullptr;
    ui::TextField *m_input = nullptr;
    SpotlightClearButton *m_clearButton = nullptr;

    std::vector<SpotlightSearchResult> m_results;
    std::vector<ui::View *> m_listItems;
    std::vector<SpotlightResultRow *> m_rows;
    std::vector<SpotlightSectionHeader *> m_headers;
    SpotlightPreviewPane *m_previewPane = nullptr;

    int m_selectedIndex = 0;
    double m_scrollAccumulator = 0.0;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_VIEW_H_
