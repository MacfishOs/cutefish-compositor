// Copyright 2012 The Chromium Authors
// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/shelf/shelf_layout_manager.{h,cc}
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#include "wm/dock_layout_manager.h"

#include <algorithm>
#include <cmath>

#include "compositor/compositor.h"
#include "input/seat.h"
#include "wm/activation.h"
#include "wm/overview/overview_controller.h"
#include "wm/window.h"
#include "wm/window_manager.h"

namespace cutefish::wm {

namespace {

// The dock and the panels: everything the shell pins along a screen edge.
bool isEdgeSurface(const Window *window)
{
    return (window->role() == WindowRole::Dock || window->role() == WindowRole::Panel)
            && !window->isPopup();
}

// A window the workspace is made of: what the dock makes room for.
bool isWorkspaceWindow(const Window *window)
{
    return window->role() == WindowRole::Normal && !window->isPopup() && window->isVisible();
}

// A dialog over a fullscreen window keeps the workspace fullscreen.
// Upstream: FindFullscreenOrPinnedWindow.
bool isFullscreenOrOverFullscreen(const Window *window)
{
    for (; window; window = window->transientParent()) {
        if (window->stateType() == WindowStateType::Fullscreen)
            return true;
    }
    return false;
}

// A dock lies long along its edge and thin across it, so the nearest edge in
// the thin direction is the one it belongs to.
ScreenEdge edgeFor(const QRect &bounds, const QRect &output)
{
    if (bounds.width() >= bounds.height()) {
        return bounds.top() - output.top() < output.bottom() - bounds.bottom() ? ScreenEdge::Top
                                                                                : ScreenEdge::Bottom;
    }
    return bounds.left() - output.left() < output.right() - bounds.right() ? ScreenEdge::Left
                                                                            : ScreenEdge::Right;
}

// Where the dock is drawn relative to bounds() once it has left the output.
QPoint hiddenOffset(const QRect &bounds, const QRect &output, ScreenEdge edge)
{
    switch (edge) {
    case ScreenEdge::Top:
        return QPoint(0, output.top() - bounds.bottom() - 1);
    case ScreenEdge::Bottom:
        return QPoint(0, output.bottom() + 1 - bounds.top());
    case ScreenEdge::Left:
        return QPoint(output.left() - bounds.right() - 1, 0);
    case ScreenEdge::Right:
        return QPoint(output.right() + 1 - bounds.left(), 0);
    }
    return QPoint();
}

QRect revealStrip(const QRect &output, ScreenEdge edge)
{
    const int px = DockLayoutManager::kRevealStripPx;
    switch (edge) {
    case ScreenEdge::Top:
        return QRect(output.left(), output.top(), output.width(), px);
    case ScreenEdge::Bottom:
        return QRect(output.left(), output.bottom() - px + 1, output.width(), px);
    case ScreenEdge::Left:
        return QRect(output.left(), output.top(), px, output.height());
    case ScreenEdge::Right:
        return QRect(output.right() - px + 1, output.top(), px, output.height());
    }
    return QRect();
}

// The dock plus the gap between it and its edge: crossing that gap on the way
// in or out must not toggle the dock. Upstream keeps the shelf shown while the
// cursor is anywhere in its bounds; the gap is a Cutefish addition, since the
// dock floats a few pixels off the edge.
QRect shownRegion(const QRect &bounds, const QRect &output, ScreenEdge edge)
{
    QRect region = bounds;
    switch (edge) {
    case ScreenEdge::Top:
        region.setTop(output.top());
        break;
    case ScreenEdge::Bottom:
        region.setBottom(output.bottom());
        break;
    case ScreenEdge::Left:
        region.setLeft(output.left());
        break;
    case ScreenEdge::Right:
        region.setRight(output.right());
        break;
    }
    return region;
}

double easeOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}

} // namespace

DockLayoutManager::DockLayoutManager(WindowManager *manager, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
    connect(m_manager, &WindowManager::windowAdded, this, &DockLayoutManager::onWindowAdded);
    connect(m_manager, &WindowManager::windowRemoved, this, &DockLayoutManager::onWindowRemoved);
    connect(m_manager, &WindowManager::activeWindowChanged, this, &DockLayoutManager::update);
    connect(m_manager, &WindowManager::pointerMoved, this, &DockLayoutManager::onPointerMoved);
    if (input::Seat *seat = m_manager->seat())
        m_pointer = seat->pointerLocation();
    for (Window *window : m_manager->windows())
        window->addObserver(this);
}

DockLayoutManager::~DockLayoutManager()
{
    for (Window *window : m_manager->windows()) {
        if (window->hasObserver(this))
            window->removeObserver(this);
    }
}

void DockLayoutManager::setBehavior(DockAutoHideBehavior behavior)
{
    if (behavior == m_behavior)
        return;
    m_behavior = behavior;
    update();
}

void DockLayoutManager::setSessionStarting(bool starting)
{
    if (starting == m_sessionStarting)
        return;
    m_sessionStarting = starting;
    update();
}

void DockLayoutManager::setSessionEnding(bool ending)
{
    if (ending == m_sessionEnding)
        return;
    m_sessionEnding = ending;
    update();
}

DockVisibilityState DockLayoutManager::visibilityState(const Window *dock) const
{
    const Dock *entry = dockFor(dock);
    return entry ? entry->visibility : DockVisibilityState::Visible;
}

DockAutoHideState DockLayoutManager::autoHideState(const Window *dock) const
{
    const Dock *entry = dockFor(dock);
    return entry ? entry->autoHide : DockAutoHideState::Shown;
}

bool DockLayoutManager::isSliding(const Window *dock) const
{
    const Dock *entry = dockFor(dock);
    return entry && entry->sliding;
}

DockLayoutManager::Dock *DockLayoutManager::dockFor(Window *window)
{
    const auto it = m_docks.find(window);
    return it == m_docks.end() ? nullptr : it->second.get();
}

const DockLayoutManager::Dock *DockLayoutManager::dockFor(const Window *window) const
{
    const auto it = m_docks.find(const_cast<Window *>(window));
    return it == m_docks.end() ? nullptr : it->second.get();
}

// Upstream: WorkspaceController::GetWindowState. The active window decides
// fullscreen when it is on this output; otherwise the topmost window there.
WorkspaceState DockLayoutManager::workspaceStateFor(const QRect &output) const
{
    const auto &windows = m_manager->windows();
    auto onOutput = [&](const Window *window) {
        return isWorkspaceWindow(window) && m_manager->outputBoundsFor(window) == output;
    };

    const Window *topmost = m_manager->activation()->activeWindow();
    if (!topmost || !onOutput(topmost)) {
        topmost = nullptr;
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            if (onOutput(*it)) {
                topmost = *it;
                break;
            }
        }
    }
    if (topmost && isFullscreenOrOverFullscreen(topmost))
        return WorkspaceState::Fullscreen;

    for (const Window *window : windows) {
        if (onOutput(window) && window->stateType() == WindowStateType::Maximized)
            return WorkspaceState::Maximized;
    }
    return WorkspaceState::Default;
}

bool DockLayoutManager::hasVisibleWindow(const QRect &output) const
{
    const auto &windows = m_manager->windows();
    return std::any_of(windows.begin(), windows.end(), [&](const Window *window) {
        return isWorkspaceWindow(window) && m_manager->outputBoundsFor(window) == output;
    });
}

bool DockLayoutManager::isLauncherShowing(const QRect &output) const
{
    const auto &windows = m_manager->windows();
    return std::any_of(windows.begin(), windows.end(), [&](const Window *window) {
        return window->role() == WindowRole::Launcher && !window->isPopup() && window->isVisible()
                && m_manager->outputBoundsFor(window) == output;
    });
}

// Upstream: ShelfLayoutManager::CalculateShelfVisibility. A fullscreen window
// gets the dock out of its way whatever the setting; unlike upstream it still
// comes back from the edge, so the dock is never out of reach. A panel has no
// setting: fullscreen is the only thing that moves it.
DockVisibilityState DockLayoutManager::computeVisibility(const Window *dock,
                                                         const QRect &output) const
{
    const WorkspaceState workspace = workspaceStateFor(output);
    if (isAwayForSession() || workspace == WorkspaceState::Fullscreen)
        return DockVisibilityState::AutoHide;
    if (dock->role() == WindowRole::Panel)
        return DockVisibilityState::Visible;
    switch (m_behavior) {
    case DockAutoHideBehavior::Never:
        return DockVisibilityState::Visible;
    case DockAutoHideBehavior::Always:
        return DockVisibilityState::AutoHide;
    case DockAutoHideBehavior::WhenObstructed:
        return workspace == WorkspaceState::Maximized ? DockVisibilityState::AutoHide
                                                      : DockVisibilityState::Visible;
    }
    return DockVisibilityState::Visible;
}

// Upstream: ShelfLayoutManager::CalculateAutoHideState.
DockAutoHideState DockLayoutManager::computeAutoHideState(const Window *dock, const Dock &entry,
                                                          const QRect &output, bool revealing) const
{
    if (entry.visibility != DockVisibilityState::AutoHide)
        return DockAutoHideState::Shown;

    if (isAwayForSession())
        return DockAutoHideState::Hidden;

    if (isLauncherShowing(output))
        return DockAutoHideState::Shown;

    // A menu or tooltip of the dock's is being read.
    const auto &children = dock->transientChildren();
    if (std::any_of(children.begin(), children.end(), [](const Window *child) {
            return child->isVisible();
        })) {
        return DockAutoHideState::Shown;
    }

    // Nothing to make room for.
    if (!hasVisibleWindow(output))
        return DockAutoHideState::Shown;

    if (m_pointer) {
        const QRect region = shownRegion(dock->bounds(), output, entry.edge);
        if (revealing && region.contains(m_pointer->toPoint()))
            return DockAutoHideState::Shown;
        if (revealStrip(output, entry.edge).contains(m_pointer->toPoint()))
            return DockAutoHideState::Shown;
    }

    return DockAutoHideState::Hidden;
}

void DockLayoutManager::update()
{
    for (Window *window : m_manager->windows()) {
        if (!isEdgeSurface(window))
            continue;
        auto &entry = m_docks[window];
        if (!entry) {
            entry = std::make_unique<Dock>();
            entry->revealTimer.setSingleShot(true);
            entry->revealTimer.setInterval(kRevealDelayMs);
            connect(&entry->revealTimer, &QTimer::timeout, this, [this, window] {
                revealNow(window);
            });
            enter(window, *entry);
        }
        updateDock(window, *entry);
    }
}

void DockLayoutManager::enter(Window *dock, Dock &entry)
{
    const QRect output = m_manager->outputBoundsFor(dock);
    if (output.isEmpty())
        return;
    entry.edge = edgeFor(dock->bounds(), output);
    entry.visibility = computeVisibility(dock, output);
    entry.autoHide = computeAutoHideState(dock, entry, output, false);
    entry.offset = hiddenOffset(dock->bounds(), output, entry.edge);
    entry.parked = true;
    present(dock, entry);
    if (entry.autoHide == DockAutoHideState::Shown)
        slideTo(dock, entry, QPoint());
}

// Upstream: ShelfLayoutManager::UpdateAutoHideStateNow.
void DockLayoutManager::revealNow(Window *dock)
{
    Dock *entry = dockFor(dock);
    if (!entry || !m_manager->hasWindow(dock))
        return;
    const QRect output = m_manager->outputBoundsFor(dock);
    if (output.isEmpty())
        return;
    const DockAutoHideState autoHide = computeAutoHideState(dock, *entry, output, true);
    if (autoHide != entry->autoHide)
        applyAutoHideState(dock, *entry, autoHide);
}

// Upstream: ShelfLayoutManager::UpdateVisibilityState and UpdateAutoHideState.
void DockLayoutManager::updateDock(Window *dock, Dock &entry)
{
    const QRect output = m_manager->outputBoundsFor(dock);
    if (output.isEmpty())
        return;
    entry.edge = edgeFor(dock->bounds(), output);
    entry.visibility = computeVisibility(dock, output);

    // While the reveal delay runs the pointer may already have moved on from
    // the strip to where the dock is about to appear.
    const bool revealing =
            entry.autoHide == DockAutoHideState::Shown || entry.revealTimer.isActive();
    const DockAutoHideState autoHide = computeAutoHideState(dock, entry, output, revealing);
    if (autoHide == entry.autoHide) {
        entry.revealTimer.stop();
        // The dock may have moved or grown while parked off screen.
        if (autoHide == DockAutoHideState::Hidden)
            slideTo(dock, entry, hiddenOffset(dock->bounds(), output, entry.edge));
        return;
    }
    // Hiding is immediate; showing waits, so a pointer merely passing along
    // the edge does not pull the dock out.
    if (autoHide == DockAutoHideState::Hidden || entry.visibility == DockVisibilityState::Visible) {
        entry.revealTimer.stop();
        applyAutoHideState(dock, entry, autoHide);
    } else if (!entry.revealTimer.isActive()) {
        entry.revealTimer.start();
    }
}

void DockLayoutManager::applyAutoHideState(Window *dock, Dock &entry, DockAutoHideState state)
{
    entry.autoHide = state;
    const QRect output = m_manager->outputBoundsFor(dock);
    slideTo(dock, entry,
            state == DockAutoHideState::Hidden ? hiddenOffset(dock->bounds(), output, entry.edge)
                                               : QPoint());
}

void DockLayoutManager::slideTo(Window *dock, Dock &entry, const QPoint &offset)
{
    if (entry.sliding ? offset == entry.slideTo : offset == entry.offset)
        return;
    // A reversal mid-slide turns around from where the dock is.
    entry.slideFrom = entry.offset;
    entry.slideStarted = -1;
    entry.sliding = true;
    entry.slideTo = offset;
    // A parked dock comes back into the scene before it moves.
    entry.parked = false;
    present(dock, entry);
    requestFrame();
}

void DockLayoutManager::present(Window *dock, const Dock &entry)
{
    QPoint offset = entry.offset;
    bool hidden = entry.parked;
    if (dock->role() == WindowRole::Panel && m_overviewProgress > 0) {
        const QPoint away =
                hiddenOffset(dock->bounds(), m_manager->outputBoundsFor(dock), entry.edge);
        offset += (QPointF(away - offset) * m_overviewProgress).toPoint();
        hidden = hidden || m_overviewProgress >= 1;
    }
    dock->setPresentationOffset(offset);
    dock->setPresentationHidden(hidden);
}

void DockLayoutManager::advance(double frameTimeMs)
{
    const double overview = m_manager->overview()->shownProgress(frameTimeMs);
    const bool overviewMoved = overview != m_overviewProgress;
    m_overviewProgress = overview;
    bool sliding = false;
    for (auto &[dock, entry] : m_docks) {
        if (!entry->sliding) {
            if (overviewMoved)
                present(dock, *entry);
            continue;
        }
        if (entry->slideStarted < 0)
            entry->slideStarted = frameTimeMs;
        const double progress = easeOut((frameTimeMs - entry->slideStarted) / kSlideDurationMs);
        const QPointF travel = entry->slideTo - entry->slideFrom;
        entry->offset = entry->slideFrom + (travel * progress).toPoint();
        if (progress >= 1) {
            entry->sliding = false;
            entry->parked = entry->autoHide == DockAutoHideState::Hidden;
        } else {
            sliding = true;
        }
        present(dock, *entry);
    }
    if (sliding)
        requestFrame();
}

void DockLayoutManager::requestFrame()
{
    if (auto *compositor = m_manager->compositor())
        compositor->scheduleRepaint();
}

void DockLayoutManager::onWindowAdded(Window *window)
{
    window->addObserver(this);
    update();
}

void DockLayoutManager::onWindowRemoved(Window *window)
{
    if (window->hasObserver(this))
        window->removeObserver(this);
    m_docks.erase(window);
    update();
}

void DockLayoutManager::onPointerMoved(const QPointF &location)
{
    m_pointer = location;
    update();
}

void DockLayoutManager::onWindowBoundsChanged(Window *, const QRect &)
{
    update();
}

void DockLayoutManager::onWindowStateTypeChanged(Window *, WindowStateType)
{
    update();
}

void DockLayoutManager::onWindowVisibilityChanged(Window *, bool)
{
    update();
}

void DockLayoutManager::onWindowTransientParentChanged(Window *, Window *)
{
    update();
}

} // namespace cutefish::wm
