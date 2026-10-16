// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/launcher/launcher_window.h"

#include <algorithm>
#include <cmath>

#include "core/main_loop.h"
#include "shell/dock/dock.h"
#include "shell/launcher/launcher.h"
#include "shell/launcher/launcher_view.h"
#include "shell/launcher/launcher_wallpaper.h"
#include "shell/shell_window.h"
#include "ui/embedded_window.h"
#include "wm/window.h"
#include "wm/window_manager.h"

namespace cutefish::shell {

LauncherWindow::LauncherWindow(Shell *shell, std::unique_ptr<Launcher> launcher, Dock *dock,
                               Wallpaper *wallpaper)
    : m_shell(shell)
    , m_launcher(std::move(launcher))
    , m_dock(dock)
    , m_wallpaper(std::make_unique<LauncherWallpaper>(wallpaper))
{
    m_window = std::make_unique<ShellWindow>(shell, wm::WindowRole::Launcher);
    auto view = std::make_unique<LauncherView>(*m_launcher, *m_wallpaper);
    m_view = view.get();
    m_window->setContentView(std::move(view));
    // Transparent: until the wallpaper decodes, the dimmer darkens what is
    // behind it.
    m_window->window()->setBackgroundColor(ui::Color {0, 0, 0, 0});
    m_window->window()->onActiveChanged = [this] { activeChanged(); };
    m_window->onCloseRequested = [this] { m_launcher->hide(); };

    m_launcher->showWindow = [this] { showWindow(); };
    m_launcher->hideWindow = [this] { hideWindow(); };
    LauncherModel &model = m_launcher->applications();
    // Applications opened before the view existed are no longer new.
    model.clearNewInstalled(m_launcher->takeOpenedApplications());
    m_subscriptions.push_back(model.applicationLaunched.connect([this] { m_launcher->hide(); }));
    m_subscriptions.push_back(m_view->dismissRequested.connect([this] { m_launcher->hide(); }));
    m_subscriptions.push_back(m_shell->outputsChanged.connect([this] { updateGeometry(); }));
    if (m_dock)
        m_subscriptions.push_back(m_dock->primaryGeometryChanged.connect([this] { updateGeometry(); }));

    // Laid out and decoded while hidden, so the first show has its pages.
    updateGeometry();
}

LauncherWindow::~LauncherWindow()
{
    *m_alive = false;
    m_subscriptions.clear();
    m_launcher->showWindow = nullptr;
    m_launcher->hideWindow = nullptr;
    m_window.reset();
    m_wallpaper.reset();
    m_launcher.reset();
}

void LauncherWindow::updateGeometry()
{
    const Shell::Output *output = m_shell->output({});
    if (!output)
        return;
    // The whole output, not its work area: it covers the status bar.
    m_window->setOutput(output->name);
    m_window->setGeometry(output->bounds);
    // The wallpaper is decoded at the device pixel size.
    m_wallpaper->setSurfaceSize(base::Size(int(std::lround(output->bounds.width() * output->scale)),
                                           int(std::lround(output->bounds.height() * output->scale))));

    // Room for the dock on its edge, and a constant 8 at the top.
    LauncherMetrics::Margins margins;
    const base::Rect dock = m_dock && m_dock->primaryGeometry ? m_dock->primaryGeometry() : base::Rect();
    if (!dock.isEmpty()) {
        switch (m_dock->settings().direction()) {
        case DockSettings::Left:
            margins.left = dock.width();
            break;
        case DockSettings::Right:
            margins.right = dock.width();
            break;
        case DockSettings::Bottom:
            margins.bottom = dock.height();
            break;
        }
    }
    m_view->setMargins(margins);
    m_view->preloadIcons();

    base::Rect available = m_shell->workArea(*output);
    available.translate(-output->bounds.x(), -output->bounds.y());
    m_view->setAvailableGeometry(ui::RectF {double(available.x()), double(available.y()),
                                            double(available.width()), double(available.height())});
}

void LauncherWindow::showWindow()
{
    updateGeometry();
    m_view->setShown(true);
    m_window->show();
    // The trusted shell needs no activation token: the window manager's own
    // activation, as cutefish_shell_surface_v1.activate.
    m_window->activate();
}

void LauncherWindow::hideWindow()
{
    m_view->setShown(false);
    m_window->hide();
}

bool LauncherWindow::keepsShownWhileInactive() const
{
    if (m_launcher->isDragging() || m_view->isMenuOpen())
        return true;
    // The capture tools take the keyboard to picture the launcher, not to close it.
    const auto &windows = m_shell->windowManager()->windows();
    return std::any_of(windows.begin(), windows.end(), [](const wm::Window *window) {
        return window->role() == wm::WindowRole::Capture && window->isVisible();
    });
}

void LauncherWindow::activeChanged()
{
    ui::Window *window = m_window->window();
    if (!window || window->isActive() || !m_launcher->isShown() || keepsShownWhileInactive())
        return;
    // A turn later: activation can pass through other windows on its way to
    // one of the launcher's own.
    std::weak_ptr<bool> alive = m_alive;
    core::mainLoop()->post([this, alive] {
        if (alive.expired())
            return;
        ui::Window *window = m_window->window();
        if (!window || window->isActive() || !m_launcher->isShown() || keepsShownWhileInactive())
            return;
        m_launcher->deactivated();
    });
}

} // namespace cutefish::shell
