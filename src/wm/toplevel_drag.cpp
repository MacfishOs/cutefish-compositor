// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/toplevel_drag.h"

#include "clipboard/selection.h"
#include "compositor/surface.h"
#include "core/logging.h"
#include "input/hit_test.h"
#include "input/seat.h"
#include "scene/node.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"

namespace cutefish::wm {

namespace {

scene::WindowAnimation *animationFor(Window *window)
{
    return dynamic_cast<scene::WindowAnimation *>(window->node());
}

} // namespace

ToplevelDrag::ToplevelDrag(WindowManager *manager, std::shared_ptr<clipboard::DataSource> source)
    : m_manager(manager)
    , m_source(std::move(source))
{
    if (input::Seat *seat = m_manager->seat()) {
        connect(seat->drag(), &input::DragController::started, this,
                &ToplevelDrag::onDragStarted);
    }
    connect(m_manager, &WindowManager::windowAdded, this, &ToplevelDrag::onWindowAdded);
    connect(m_manager, &WindowManager::windowRemoved, this, &ToplevelDrag::onWindowRemoved);
}

ToplevelDrag::~ToplevelDrag()
{
    // Only a client going away mid-drag gets here with the drag running; the
    // drag carries on without its window.
    if (input::Seat *seat = m_manager->seat()) {
        if (seat->drag()->windowDelegate() == this)
            seat->drag()->setWindowDelegate(nullptr);
    }
    stopMove();
    detach();
}

void ToplevelDrag::attach(compositor::Surface *surface, const QPoint &offset)
{
    detach();
    m_surface = surface;
    m_offset = offset;
    m_surface->addSurfaceObserver(this);
    // An existing window moves from where it is, as a caption drag would.
    if (Window *window = windowForSurface(); window && m_dragging)
        startMove(window);
}

void ToplevelDrag::onDragStarted()
{
    input::Seat *seat = m_manager->seat();
    if (m_dragging || !m_source || seat->drag()->source() != m_source)
        return;
    m_dragging = true;
    seat->drag()->setWindowDelegate(this);
    if (Window *window = windowForSurface())
        startMove(window);
}

void ToplevelDrag::onWindowAdded(Window *window)
{
    if (!m_surface || window->rootSurface() != m_surface)
        return;
    // Mapping it is the moment it appears under the pointer: with no open
    // animation, at the offset the client asked for.
    if (auto *animation = animationFor(window)) {
        m_animationStyle = animation->style();
        animation->setStyle(scene::WindowAnimation::Style::None);
    }
    if (input::Seat *seat = m_manager->seat()) {
        const QPoint origin = seat->pointerLocation().toPoint() - m_offset;
        window->setBounds(window->bounds().translated(origin - window->contentBounds().topLeft()));
    }
    if (m_dragging)
        startMove(window);
}

void ToplevelDrag::onWindowRemoved(Window *window)
{
    if (window == m_moving)
        m_moving = nullptr;
    if (!m_surface || window->rootSurface() != m_surface)
        return;
    detach();
    if (Window *target = m_manager->windowById(std::exchange(m_dropWindow, 0)))
        target->activate();
}

Window *ToplevelDrag::windowForSurface() const
{
    if (!m_surface)
        return nullptr;
    for (Window *window : m_manager->windows()) {
        if (window->rootSurface() == m_surface)
            return window;
    }
    return nullptr;
}

void ToplevelDrag::startMove(Window *window)
{
    input::Seat *seat = m_manager->seat();
    if (!seat || m_moving == window)
        return;
    stopMove();
    if (!m_manager->eventHandler()->beginMove(window, seat->pointerLocation())) {
        qCDebug(cfCompositor) << "toplevel drag could not move" << window;
        return;
    }
    m_moving = window;
    if (scene::Node *node = window->node())
        node->setEventTargetingBlocked(true);
}

void ToplevelDrag::stopMove()
{
    Window *window = std::exchange(m_moving, nullptr);
    if (!window)
        return;
    if (scene::Node *node = window->node())
        node->setEventTargetingBlocked(false);
    ToplevelEventHandler *handler = m_manager->eventHandler();
    if (handler->draggedWindow() == window)
        handler->completeDrag(ToplevelEventHandler::DragResult::Revert);
}

void ToplevelDrag::dragMoved(const QPointF &location)
{
    ToplevelEventHandler *handler = m_manager->eventHandler();
    if (m_moving && handler->draggedWindow() == m_moving)
        handler->onPointerMotion(location);
}

void ToplevelDrag::dragEnded(bool released)
{
    m_dragging = false;
    Window *window = std::exchange(m_moving, nullptr);
    // Looked up while the dragged window still takes no part in the hit test.
    if (window && released) {
        if (input::Seat *seat = m_manager->seat()) {
            QPointF local;
            Window *below = m_manager->windowForSurface(input::findTargetSurface(
                    m_manager->compositor(), seat->pointerLocation(), &local));
            if (below && below != window && below->processId() > 0
                && below->processId() == window->processId())
                m_dropWindow = below->id();
        }
    }
    if (window) {
        if (scene::Node *node = window->node())
            node->setEventTargetingBlocked(false);
        ToplevelEventHandler *handler = m_manager->eventHandler();
        if (handler->draggedWindow() == window) {
            handler->completeDrag(released ? ToplevelEventHandler::DragResult::Success
                                           : ToplevelEventHandler::DragResult::Revert);
        }
    }
    if (Window *attached = windowForSurface(); attached && m_animationStyle) {
        if (auto *animation = animationFor(attached))
            animation->setStyle(*m_animationStyle);
    }
    m_animationStyle.reset();
}

void ToplevelDrag::onSurfaceDestroying(compositor::Surface *surface)
{
    if (surface == m_surface)
        detach();
}

void ToplevelDrag::detach()
{
    if (!m_surface)
        return;
    if (Window *window = windowForSurface(); window && window == m_moving)
        stopMove();
    if (Window *window = windowForSurface(); window && m_animationStyle) {
        if (auto *animation = animationFor(window))
            animation->setStyle(*m_animationStyle);
    }
    m_animationStyle.reset();
    m_surface->removeSurfaceObserver(this);
    m_surface = nullptr;
}

} // namespace cutefish::wm
