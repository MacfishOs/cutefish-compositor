// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/spotlight/spotlight_window.h"

#include <algorithm>
#include <cmath>

#include "base/event_loop.h"
#include "core/main_loop.h"
#include "shell/controlcenter/panel_style.h"
#include "shell/shell.h"
#include "shell/shell_data.h"
#include "shell/shell_window.h"
#include "ui/embedded_window.h"
#include "ui/theme.h"
#include "wm/window.h"

namespace cutefish::shell {

SpotlightWindow::SpotlightWindow(Shell *shell, std::unique_ptr<Spotlight> spotlight)
    : m_shell(shell)
    , m_spotlight(std::move(spotlight))
{
    m_window = std::make_unique<ShellWindow>(shell, wm::WindowRole::Launcher);
    auto view = std::make_unique<SpotlightView>(shell, m_spotlight.get());
    m_view = view.get();
    m_view->onHeightChanged = [this] { updateGeometry(); };
    m_window->setContentView(std::move(view));

    m_window->window()->setBackgroundColor(ui::Color {0, 0, 0, 0});
    m_window->window()->onActiveChanged = [this] { activeChanged(); };
    m_window->onCloseRequested = [this] { m_spotlight->hide(); };

    m_spotlight->showWindow = [this] { showWindow(); };
    m_spotlight->hideWindow = [this] { hideWindow(); };
    m_spotlight->registerService();

    m_subscriptions.push_back(m_shell->outputsChanged.connect([this] { updateGeometry(); }));
    updateGeometry();
}

SpotlightWindow::~SpotlightWindow()
{
    *m_alive = false;
    m_subscriptions.clear();
    m_spotlight->showWindow = nullptr;
    m_spotlight->hideWindow = nullptr;
    m_window.reset();
    m_spotlight.reset();
}

void SpotlightWindow::updateGeometry()
{
    const Shell::Output *output = m_shell->output({});
    if (!output)
        return;

    const double width = SpotlightView::kWidth;
    const double height = m_view->desiredHeight();
    const double x = output->bounds.x() + (output->bounds.width() - width) / 2.0;
    const double y = output->bounds.y() + output->bounds.height() * 0.18;

    const base::Rect bounds(int(std::lround(x)), int(std::lround(y)),
                            int(std::lround(width)), int(std::lround(height)));

    m_window->setOutput(output->name);
    m_window->setGeometry(bounds);

    ui::Path shape;
    shape.addRoundedRect({0, 0, width, height}, 18.0, 18.0);
    const bool dark = ui::Theme::current().darkMode;
    render::GlassParams glass = dark ? popupGlass(0.12, 0.12, 0.14, 0.85)
                                     : popupGlass(0.95, 0.95, 0.97, 0.85);
    glass.saturation = 2.2f;
    m_window->setBackgroundBlur({0, 0, int(width), int(height)}, 24, glass, shape);
}

void SpotlightWindow::showWindow()
{
    m_view->resetSearch();
    updateGeometry();
    m_window->show();
    m_window->activate();
    m_view->focusInput();
}

void SpotlightWindow::hideWindow()
{
    m_window->hide();
}

void SpotlightWindow::activeChanged()
{
    ui::Window *window = m_window->window();
    if (!window || window->isActive() || !m_spotlight->isShown())
        return;
    std::weak_ptr<bool> alive = m_alive;
    core::mainLoop()->post([this, alive] {
        if (alive.expired())
            return;
        ui::Window *window = m_window->window();
        if (!window || window->isActive() || !m_spotlight->isShown())
            return;
        m_spotlight->hide();
    });
}

} // namespace cutefish::shell
