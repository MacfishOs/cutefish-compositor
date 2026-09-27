// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/pointer.cc (pointer constraints)
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "input/pointer_constraints.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "compositor/surface.h"
#include "core/logging.h"
#include "input/drag.h"
#include "input/hit_test.h"
#include "input/pointer_constraint_delegate.h"
#include "input/seat.h"
#include "input/session_gate.h"

namespace cutefish::input {

namespace {

// Hit tests floor the location, so this keeps a confined pointer on the last
// pixel of the region, and is the smallest step a client can see.
constexpr double kEdgeInset = 1.0 / 256;

QPoint flooredPoint(const QPointF &point)
{
    return QPoint(static_cast<int>(std::floor(point.x())), static_cast<int>(std::floor(point.y())));
}

const compositor::Surface *rootSurface(const compositor::Surface *surface)
{
    while (surface && surface->parentSurface())
        surface = surface->parentSurface();
    return surface;
}

QPointF clampToRect(const QPointF &point, const QRect &rect)
{
    return QPointF(std::clamp(point.x(), double(rect.left()), rect.left() + rect.width() - kEdgeInset),
                   std::clamp(point.y(), double(rect.top()), rect.top() + rect.height() - kEdgeInset));
}

} // namespace

PointerConstraints::PointerConstraints(Seat *seat)
    : m_seat(seat)
{
}

PointerConstraints::~PointerConstraints()
{
    for (auto &[surface, delegate] : std::exchange(m_constraints, {})) {
        surface->removeSurfaceObserver(this);
        delegate->onDefunct();
    }
    m_active = nullptr;
}

bool PointerConstraints::add(PointerConstraintDelegate *delegate)
{
    compositor::Surface *surface = delegate->constrainedSurface();
    if (!surface) {
        delegate->onDefunct();
        return false;
    }

    // Can only have one constraint request per surface.
    const auto [it, inserted] = m_constraints.try_emplace(surface, delegate);
    if (!inserted) {
        qCDebug(cfInput) << "Pointer constraint not granted; one already exists.";
        delegate->onAlreadyConstrained();
        delegate->onDefunct();
        return false;
    }
    surface->addSurfaceObserver(this);
    update();
    return true;
}

void PointerConstraints::remove(PointerConstraintDelegate *delegate)
{
    // Destroying the lock is how a client unlocks, Xwayland included, so the
    // hint applies here too.
    std::optional<QPointF> warpTo;
    if (m_active == delegate) {
        warpTo = hintPosition();
        m_active = nullptr;
    }
    erase(delegate);
    if (warpTo && warp)
        warp(*warpTo);
}

std::optional<QPointF> PointerConstraints::hintPosition() const
{
    if (!isLocked())
        return std::nullopt;
    const auto hint = m_active->cursorPositionHint();
    const compositor::Surface *surface = m_active->constrainedSurface();
    if (!hint || !surface)
        return std::nullopt;
    const QPointF location = m_seat->cursorPosition();
    return location - locationInSurface(surface, location) + *hint;
}

void PointerConstraints::erase(PointerConstraintDelegate *delegate)
{
    compositor::Surface *surface = delegate->constrainedSurface();
    const auto it = m_constraints.find(surface);
    if (it == m_constraints.end() || it->second != delegate)
        return;
    m_constraints.erase(it);
    surface->removeSurfaceObserver(this);
}

bool PointerConstraints::isLocked() const
{
    return m_active && m_active->kind() == PointerConstraintDelegate::Kind::Lock;
}

QRegion PointerConstraints::effectiveRegion(PointerConstraintDelegate *delegate) const
{
    const compositor::Surface *surface = delegate->constrainedSurface();
    if (!surface)
        return {};
    const QRegion input = surface->localHitTestRegion();
    const auto region = delegate->region();
    return region ? input & *region : input;
}

bool PointerConstraints::canActivate(PointerConstraintDelegate *delegate) const
{
    if (!m_capturePermitted || m_seat->wasShutdown() || m_seat->drag()->grabsInput())
        return false;
    if (auto *gate = m_seat->sessionGate(); gate && gate->restrictsInput())
        return false;

    compositor::Surface *surface = delegate->constrainedSurface();
    if (!surface || !surface->isMapped() || surface->isHidden()
        || m_seat->pointerFocus() != surface)
        return false;

    // As in Exo, only the active window may capture the pointer; the keyboard
    // focus is how the seat knows which window that is.
    const compositor::Surface *focus = m_seat->focusedSurface();
    if (!focus || rootSurface(focus) != rootSurface(surface))
        return false;

    // Once active, a lock keeps the pointer where it was and a confinement
    // keeps it inside, so only activation looks at the location.
    if (delegate == m_active)
        return true;
    const QPointF location = locationInSurface(surface, m_seat->cursorPosition());
    return effectiveRegion(delegate).contains(flooredPoint(location));
}

void PointerConstraints::update()
{
    if (m_active && !canActivate(m_active)) {
        const std::optional<QPointF> warpTo = hintPosition();
        deactivate();
        // The warp's motion re-enters this, and may activate another constraint.
        if (warpTo && warp)
            warp(*warpTo);
    }
    if (m_active)
        return;

    const auto it = m_constraints.find(m_seat->pointerFocus());
    if (it == m_constraints.end() || !canActivate(it->second))
        return;
    m_active = it->second;
    m_active->onConstraintActivated();
}

void PointerConstraints::deactivate()
{
    PointerConstraintDelegate *delegate = std::exchange(m_active, nullptr);
    if (!delegate)
        return;
    delegate->onConstraintBroken();
    if (!delegate->isPersistent()) {
        erase(delegate);
        delegate->onDefunct();
    }
}

QPointF PointerConstraints::confine(const QPointF &from, const QPointF &to) const
{
    if (!m_active || m_active->kind() != PointerConstraintDelegate::Kind::Confine)
        return to;
    const compositor::Surface *surface = m_active->constrainedSurface();
    const QPointF origin = from - locationInSurface(surface, from);
    const QRegion region = effectiveRegion(m_active);
    if (region.isEmpty())
        return from;
    const QPointF target = to - origin;
    if (region.contains(flooredPoint(target)))
        return to;

    // Slide along the edge of the rectangle the pointer is in, or of the
    // nearest one when the window moved away from under it.
    const QPointF current = from - origin;
    QRect nearest;
    double nearestDistance = std::numeric_limits<double>::max();
    for (const QRect &rect : region) {
        const QPointF delta = current - clampToRect(current, rect);
        const double distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (distance < nearestDistance) {
            nearest = rect;
            nearestDistance = distance;
        }
    }
    return origin + clampToRect(target, nearest);
}

bool PointerConstraints::breakByUserAction()
{
    // Prevent pointer capture until the next user action that permits it, even
    // if a constraint is not active now, so an application cannot evade this
    // by rapidly toggling capture.
    m_capturePermitted = false;
    if (!m_active)
        return false;
    qCDebug(cfInput) << "Pointer constraint broken by user action";
    update();
    return true;
}

void PointerConstraints::onPointerPressed(compositor::Surface *surface)
{
    // Clicking any surface with a constraint permits capture.
    if (m_capturePermitted || !m_constraints.contains(surface))
        return;
    m_capturePermitted = true;
    update();
}

void PointerConstraints::onSurfaceDestroying(compositor::Surface *surface)
{
    const auto it = m_constraints.find(surface);
    if (it == m_constraints.end())
        return;
    PointerConstraintDelegate *delegate = it->second;
    m_constraints.erase(it);
    surface->removeSurfaceObserver(this);
    if (m_active == delegate) {
        m_active = nullptr;
        delegate->onConstraintBroken();
    }
    delegate->onDefunct();
}

} // namespace cutefish::input
