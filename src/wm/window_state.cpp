// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/window_state.h"

#include <algorithm>

#include <QImage>
#include <QPointer>
#include <QTimer>

#include "scene/window_animation.h"
#include "wm/activation.h"
#include "wm/placement.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_observer.h"
#include "wm/window_snap.h"
#include "wm/wm_event.h"

namespace cutefish::wm {

namespace {

// Upper bound for previews and high-density dock thumbnails.
constexpr int kMinimizedSnapshotSize = 384;

// A window may only restore back to a state at a strictly lower layer.
// Upstream: ash/wm/window_state.cc kWindowStateRestoreHistoryLayerMap.
int restoreLayer(WindowStateType type)
{
    switch (type) {
    case WindowStateType::Normal:
        return 0;
    case WindowStateType::PrimarySnapped:
    case WindowStateType::SecondarySnapped:
        return 1;
    case WindowStateType::Maximized:
        return 2;
    case WindowStateType::Fullscreen:
        return 3;
    case WindowStateType::Minimized:
        return 4;
    }
    return 0;
}

bool canRestoreState(WindowStateType current, WindowStateType previous)
{
    return restoreLayer(current) > restoreLayer(previous);
}

// Unmaximizing to bounds that are still the size of the work area leaves no
// visible difference and nothing to grab, so inset them slightly.
// Upstream: ash/wm/default_state.cc kMaximizedWindowInset.
constexpr int kMaximizedWindowInset = 10;

} // namespace

WindowState::WindowState(Window *window)
    : m_window(window)
{
}

WindowState::~WindowState() = default;

void WindowState::saveCurrentBoundsForRestore()
{
    m_restoreBounds = m_window->bounds();
}

bool WindowState::canSnap() const
{
    return m_window->isResizable()
            && canSnapSize(m_window->minimumOuterSize(),
                           m_window->manager()->workAreaFor(m_window));
}

WindowStateType WindowState::restoreStateType() const
{
    return m_restoreHistory.empty() ? WindowStateType::Normal : m_restoreHistory.back();
}

void WindowState::onEvent(const WMEvent &event)
{
    if (m_window->isPopup())
        return;
    switch (event.type()) {
    case WMEventType::Normal:
        enterState(WindowStateType::Normal);
        break;
    case WMEventType::Maximize:
        enterState(WindowStateType::Maximized);
        break;
    case WMEventType::Minimize:
        enterState(WindowStateType::Minimized);
        break;
    case WMEventType::Fullscreen:
        enterState(WindowStateType::Fullscreen);
        break;
    case WMEventType::SnapPrimary:
        enterState(WindowStateType::PrimarySnapped);
        break;
    case WMEventType::SnapSecondary:
        enterState(WindowStateType::SecondarySnapped);
        break;
    case WMEventType::Restore:
        enterState(restoreStateType());
        break;
    case WMEventType::ToggleMaximize:
        enterState(isMaximized() ? restoreStateType() : WindowStateType::Maximized);
        break;
    case WMEventType::ToggleFullscreen:
        enterState(isFullscreen() ? restoreStateType() : WindowStateType::Fullscreen);
        break;
    case WMEventType::SetBounds: {
        const auto &setBounds = static_cast<const SetBoundsWMEvent &>(event);
        if (isNormal()) {
            QRect bounds = setBounds.requestedBounds();
            adjustBoundsToEnsureMinimumVisibility(m_window->manager()->visibleAreaFor(m_window),
                                                  &bounds,
                                                  m_window->manager()->workAreaFor(m_window));
            setBoundsDirect(bounds);
        } else if (isMinimized()) {
            // Nothing to show, but the window still needs somewhere to come
            // back to.
            m_restoreBounds = setBounds.requestedBounds();
        }
        break;
    }
    case WMEventType::WorkAreaChanged:
        if (!m_window->manager()->outputBounds().isEmpty())
            updateBoundsFromState(m_type);
        break;
    }
}

void WindowState::enterState(WindowStateType next)
{
    if (next == WindowStateType::Maximized && !m_window->isResizable())
        return;
    if (isSnappedStateType(next) && !canSnap())
        return;
    if (next == m_type)
        return;

    const WindowStateType previous = m_type;
    const QRect previousBounds = m_window->bounds();

    // Only the normal state's bounds are worth coming back to.
    if (previous == WindowStateType::Normal)
        saveCurrentBoundsForRestore();

    m_type = next;
    updateRestoreHistory(previous);
    updateBoundsFromState(previous);
    updateVisibility(previous);

    // A hidden window, or one on an inactive desk, has nothing on screen to
    // animate, and a minimize snapshot taken now would be blank.
    auto *animation = dynamic_cast<scene::WindowAnimation *>(m_window->node());
    if (animation && !m_window->isHidden() && !m_window->isDeskHidden()) {
        using Type = scene::WindowAnimation::Type;
        // Read the dock bounds again on restore: its icon may have moved.
        QRect target = m_window->minimizedGeometry();
        if (target.isEmpty()) {
            const QRect output = m_window->manager()->outputBoundsFor(m_window);
            target = QRect(output.center().x(), output.bottom(), 1, 1);
        }
        const bool dock = next == WindowStateType::Minimized || previous == WindowStateType::Minimized;
        const QPointer<WindowManager> manager(m_window->manager());
        const quint64 id = m_window->id();
        animation->setHoldAtEnd(next == WindowStateType::Minimized);
        if (next == WindowStateType::Minimized) {
            // Landed on the dock slot: hand over to the shell's thumbnail.
            animation->setFinishedCallback([manager, id] {
                if (!manager)
                    return;
                QTimer::singleShot(0, manager.data(), [manager, id] {
                    if (Window *window = manager->windowById(id))
                        window->beginMinimizeHandoff();
                });
            });
        } else if (dock) {
            // Back into the window's own layer once it has left the panels' way.
            animation->setFinishedCallback([manager] {
                if (!manager)
                    return;
                QTimer::singleShot(0, manager.data(), [manager] {
                    manager->restackScene();
                });
            });
        } else {
            animation->setFinishedCallback({});
        }
        if (next == WindowStateType::Minimized) {
            animation->start(Type::Minimize, previousBounds, target);
            requestMinimizedSnapshot(animation);
        } else if (previous == WindowStateType::Minimized) {
            animation->start(Type::Restore, target, m_window->bounds());
        } else if (previousBounds != m_window->bounds()) {
            animation->start(Type::Bounds, previousBounds, m_window->bounds());
        }
        if (dock)
            m_window->manager()->restackScene();
    } else if (animation && previous == WindowStateType::Minimized) {
        // A leftover Minimize keeps drawing the old snapshot in place of the
        // live window once it shows.
        animation->setHoldAtEnd(false);
        animation->setFinishedCallback({});
        animation->skip(scene::WindowAnimation::Type::Restore);
    }

    // Restore bounds and restore history are two views of the same thing;
    // an empty history means there is nothing left to restore to.
    if (m_restoreHistory.empty())
        clearRestoreBounds();

    for (WindowObserver *observer : std::vector<WindowObserver *>(m_window->m_observers)) {
        if (m_window->hasObserver(observer))
            observer->onWindowStateTypeChanged(m_window, previous);
    }
}

void WindowState::updateMinimizeTarget()
{
    const QRect target = m_window->minimizedGeometry();
    auto *animation = dynamic_cast<scene::WindowAnimation *>(m_window->node());
    if (animation && isMinimized() && !target.isEmpty())
        animation->setMinimizeTarget(target);
}

void WindowState::requestMinimizedSnapshot(scene::WindowAnimation *animation)
{
    const QPointer<WindowManager> manager(m_window->manager());
    const quint64 id = m_window->id();
    animation->requestSnapshot(
            [manager, id](const QImage &image) {
                if (!manager)
                    return;
                // Rendering is not the place to notify observers.
                QTimer::singleShot(0, manager.data(), [manager, id, image] {
                    Window *window = manager->windowById(id);
                    if (!window || !window->state()->isMinimized())
                        return;
                    window->setMinimizedSnapshot(image);
                });
            },
            kMinimizedSnapshotSize);
}

void WindowState::updateRestoreHistory(WindowStateType previous)
{
    while (!m_restoreHistory.empty() && !canRestoreState(m_type, m_restoreHistory.back()))
        m_restoreHistory.pop_back();

    if (canRestoreState(m_type, previous))
        m_restoreHistory.push_back(previous);
}

void WindowState::updateBoundsFromState(WindowStateType previous)
{
    WindowManager *manager = m_window->manager();
    const QRect workArea = manager->workAreaFor(m_window);

    switch (m_type) {
    case WindowStateType::Normal: {
        QRect bounds = m_window->bounds();
        if (hasRestoreBounds()) {
            bounds = restoreBounds();
            // The restore bounds can be as large as the work area if the window
            // was resized to it, or if the output shrank while it was
            // maximized. Inset so the result is visibly not maximized. Only
            // when leaving Maximized: a minimise or fullscreen round trip
            // brings a work-area-sized window back exactly as it was.
            if (previous == WindowStateType::Maximized && !workArea.isEmpty()
                && bounds.width() >= workArea.width() && bounds.height() >= workArea.height()) {
                bounds = workArea.adjusted(kMaximizedWindowInset, kMaximizedWindowInset,
                                           -kMaximizedWindowInset, -kMaximizedWindowInset);
            }
        }
        adjustBoundsToEnsureMinimumVisibility(manager->visibleAreaFor(m_window), &bounds,
                                              workArea);
        setBoundsDirect(bounds);
        break;
    }
    case WindowStateType::PrimarySnapped:
    case WindowStateType::SecondarySnapped:
        setBoundsDirect(snapBounds(m_type == WindowStateType::PrimarySnapped ? SnapType::Primary
                                                                             : SnapType::Secondary,
                                   workArea));
        break;
    case WindowStateType::Maximized:
        setBoundsDirect(workArea);
        break;
    case WindowStateType::Fullscreen:
        setBoundsDirect(manager->outputBoundsFor(m_window));
        break;
    case WindowStateType::Minimized:
        // A minimized window keeps the bounds it will come back to.
        break;
    }
}

void WindowState::updateVisibility(WindowStateType previous)
{
    if (isMinimized()) {
        // Window::setVisible drops activation; a window nobody can see must
        // not hold the keyboard.
        m_window->setVisible(false);
    } else if (previous == WindowStateType::Minimized) {
        m_window->setMinimizedSnapshot(QImage());
        m_window->setVisible(!m_window->isHidden());
        m_window->manager()->restackScene();
    }
}

void WindowState::setBoundsDirect(const QRect &bounds)
{
    QRect adjusted = bounds;
    // Below the minimum size nothing useful is left to interact with, whatever
    // the state machine computed.
    if (isNormal()) {
        const QSize minimum = m_window->minimumOuterSize();
        adjusted.setWidth(std::max(adjusted.width(), minimum.width()));
        adjusted.setHeight(std::max(adjusted.height(), minimum.height()));
    }
    m_window->setBounds(adjusted);
}

} // namespace cutefish::wm
