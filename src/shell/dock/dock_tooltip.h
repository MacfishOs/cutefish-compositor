// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_DOCK_DOCK_TOOLTIP_H_
#define CUTEFISH_SHELL_DOCK_DOCK_TOOLTIP_H_

#include <memory>
#include <string>

#include "base/geometry.h"
#include "ui/geometry.h"

namespace cutefish::ui {
class Label;
class Window;
} // namespace cutefish::ui

namespace cutefish::shell {

class DockWindow;
class Shell;

// The dock's tooltip: a glass bubble whose arrow points at
// the icon, in a still popup covering everywhere a tip can appear along the
// dock, so moving to the next icon moves the bubble inside it rather than
// moving a window.
class DockTooltip
{
public:
    DockTooltip(DockWindow *dock, Shell *shell);
    ~DockTooltip();

    // `target`: the icon's centre in the dock window's coordinates.
    void show(const std::string &text, const base::PointF &target);
    void hide();
    bool isShown() const;
    const std::string &text() const { return m_text; }
    ui::Window *window() const { return m_window.get(); }

private:
    DockWindow *m_dock;
    Shell *m_shell;
    std::unique_ptr<ui::Window> m_window;
    ui::Label *m_label = nullptr;
    // Measures the text at the dock window's scale, as the label is drawn:
    // hinted text is wider at 1.5 than at 1.
    ui::Label *m_measure = nullptr;
    ui::RectF m_area;
    std::string m_text;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_DOCK_DOCK_TOOLTIP_H_
