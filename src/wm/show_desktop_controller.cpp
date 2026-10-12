// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/show_desktop_controller.h"

#include <algorithm>
#include <cmath>

#include "compositor/compositor.h"
#include "wm/overview/overview_controller.h"
#include "wm/window.h"
#include "wm/window_manager.h"

namespace cutefish::wm {

namespace {

double easeOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}

// Moved aside: the application windows on screen, dialogs included, but not
// the shell's own or popups, which belong to the window they open from.
bool isMovable(const Window *window)
{
    return window->isVisible() && !window->isShellWindow() && !window->isPopup();
}

const Window *transientRoot(const Window *window)
{
    while (window->transientParent())
        window = window->transientParent();
    return window;
}

} // namespace

ShowDesktopController::ShowDesktopController(WindowManager *manager)
    : m_manager(manager)
{
    m_manager->activation()->addObserver(this);
    m_manager->desks()->addObserver(this);
    if (compositor::Compositor *compositor = m_manager->compositor())
        connect(compositor, &compositor::Compositor::frameStarted, this,
                &ShowDesktopController::advance);
}

ShowDesktopController::~ShowDesktopController()
{
    releaseAll();
    m_manager->desks()->removeObserver(this);
    m_manager->activation()->removeObserver(this);
}

bool ShowDesktopController::isMovedAside(const Window *window) const
{
    return std::find(m_windows.begin(), m_windows.end(), window) != m_windows.end();
}

void ShowDesktopController::toggle()
{
    if (m_active)
        exit();
    else
        enter();
}

void ShowDesktopController::enter()
{
    // The overview has the screen and the windows meanwhile.
    if (m_active || !m_manager->allowsActivation() || m_manager->overview()->isActive())
        return;
    // Windows still sliding back from the last time are taken along.
    for (Window *window : m_manager->windows()) {
        if (isMovable(window) && !isMovedAside(window)) {
            window->addObserver(this);
            m_windows.push_back(window);
        }
    }
    if (m_windows.empty())
        return;
    // The desktop takes the keyboard. Activating it before the mode starts
    // keeps that activation from ending the mode.
    for (Window *window : m_manager->windows()) {
        if (window->role() == WindowRole::Desktop && window->isVisible()) {
            m_manager->activation()->activate(window);
            break;
        }
    }
    m_active = true;
    slideTo(1);
}

void ShowDesktopController::exit(Window *activate)
{
    if (!m_active)
        return;
    m_active = false;
    slideTo(0);
    if (activate)
        m_manager->activation()->activate(activate);
}

void ShowDesktopController::exitImmediately()
{
    m_active = false;
    m_sliding = false;
    m_progress = 0;
    releaseAll();
}

void ShowDesktopController::advance(double frameTimeMs)
{
    if (!m_sliding)
        return;
    if (m_slideStarted < 0)
        m_slideStarted = frameTimeMs;
    const double t = (frameTimeMs - m_slideStarted) / kSlideDurationMs;
    m_progress = m_slideFrom + (m_slideTo - m_slideFrom) * easeOut(t);
    if (t >= 1) {
        m_progress = m_slideTo;
        m_sliding = false;
    }
    present();
    if (m_sliding)
        requestFrame();
    else if (m_progress == 0)
        releaseAll();
}

void ShowDesktopController::onWindowActivated(Window *gainedActive, Window *lostActive)
{
    Q_UNUSED(lostActive);
    if (m_active && gainedActive && !gainedActive->isShellWindow())
        exit();
}

void ShowDesktopController::onDeskActivationChanged(const Desk *activated, const Desk *deactivated)
{
    Q_UNUSED(activated);
    Q_UNUSED(deactivated);
    exitImmediately();
}

void ShowDesktopController::onDeskSwitchAnimationLaunching()
{
    // Before the switch takes its frames of the desk being left.
    exitImmediately();
}

void ShowDesktopController::onWindowBoundsChanged(Window *window, const QRect &oldBounds)
{
    Q_UNUSED(window);
    Q_UNUSED(oldBounds);
    present();
}

void ShowDesktopController::onWindowVisibilityChanged(Window *window, bool visible)
{
    if (!visible)
        release(window);
}

void ShowDesktopController::onWindowDestroying(Window *window)
{
    release(window);
}

QPoint ShowDesktopController::awayOffset(const Window *window) const
{
    // A dialog goes the way of the window it belongs to, and keeps its place
    // over it.
    const Window *root = transientRoot(window);
    const QRect output = m_manager->outputBoundsFor(root);
    const QRect bounds = root->bounds();

    // An edge another output continues past would show the window there.
    const auto continues = [this, &output](int x) {
        for (const WindowManager::OutputArea &area : m_manager->outputAreas()) {
            if (area.bounds != output && area.bounds.contains(x, output.center().y()))
                return true;
        }
        return false;
    };
    bool left = bounds.center().x() < output.center().x();
    if (left && continues(output.left() - 1) && !continues(output.right() + 1))
        left = false;
    else if (!left && continues(output.right() + 1) && !continues(output.left() - 1))
        left = true;

    const int dx = left ? std::min(0, output.left() + kSliverWidth - (bounds.left() + bounds.width()))
                        : std::max(0, output.left() + output.width() - kSliverWidth - bounds.left());
    return QPoint(dx, 0);
}

void ShowDesktopController::slideTo(double target)
{
    // A reversal mid-slide turns around from where the windows are.
    m_slideFrom = m_progress;
    m_slideTo = target;
    m_slideStarted = -1;
    m_sliding = m_progress != target;
    if (m_sliding)
        requestFrame();
    else if (target == 0)
        releaseAll();
}

void ShowDesktopController::present()
{
    for (Window *window : m_windows)
        window->setPresentationOffset((QPointF(awayOffset(window)) * m_progress).toPoint());
}

void ShowDesktopController::release(Window *window)
{
    const auto it = std::find(m_windows.begin(), m_windows.end(), window);
    if (it == m_windows.end())
        return;
    m_windows.erase(it);
    window->removeObserver(this);
    window->setPresentationOffset(QPoint());
}

void ShowDesktopController::releaseAll()
{
    while (!m_windows.empty())
        release(m_windows.back());
}

void ShowDesktopController::requestFrame()
{
    if (compositor::Compositor *compositor = m_manager->compositor())
        compositor->scheduleRepaint();
}

} // namespace cutefish::wm
