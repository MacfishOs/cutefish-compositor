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

struct SpotlightSearchResult {
    enum class Kind { Application, Setting };
    Kind kind = Kind::Application;
    std::string title;
    std::string description;
    std::string icon;
    DesktopEntryPtr entry;
    std::string settingModule;
};

class SpotlightResultRow final : public ui::View
{
public:
    SpotlightResultRow();

    bool acceptsPointer() const override { return true; }

    void setData(const SpotlightSearchResult &result, bool selected);

    std::function<void()> onClicked;

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;
    void pointerPress(const ui::PointerEvent &event) override;

private:
    ui::ThemeIcon *m_icon = nullptr;
    ui::Label *m_title = nullptr;
    ui::Label *m_description = nullptr;
    bool m_selected = false;
};

class SpotlightView final : public ui::View
{
public:
    static constexpr double kWidth = 680.0;
    static constexpr double kHeaderHeight = 58.0;
    static constexpr double kRowHeight = 52.0;
    static constexpr size_t kMaxResults = 7;

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
    std::vector<SpotlightSearchResult> m_results;
    std::vector<SpotlightResultRow *> m_rows;
    int m_selectedIndex = 0;
    double m_scrollAccumulator = 0.0;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_VIEW_H_
