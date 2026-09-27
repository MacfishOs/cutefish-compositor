// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/wayland/wayland_positioner.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: gfx geometry becomes Qt geometry. The constraint
// solver itself is unchanged.

#ifndef CUTEFISH_WAYLAND_PROTOCOLS_XDG_POSITIONER_H_
#define CUTEFISH_WAYLAND_PROTOCOLS_XDG_POSITIONER_H_

#include <cstdint>

#include <QPoint>
#include <QRect>
#include <QSize>

namespace cutefish::wayland {

class WaylandPositioner
{
public:
    struct Result {
        QPoint origin;
        QSize size;

        QRect toRect() const { return QRect(origin, size); }
    };

    // The 1-dimensional projection of the gravity/anchor values.
    enum Direction { kNegative = -1, kNeutral = 0, kPositive = 1 };

    WaylandPositioner() = default;
    WaylandPositioner(const WaylandPositioner&) = delete;
    WaylandPositioner& operator=(const WaylandPositioner&) = delete;

    // `workArea` is in the same coordinate space as the anchor rect, which for
    // xdg_positioner is the parent surface's window geometry.
    Result calculateBounds(const QRect& workArea) const;

    void setSize(const QSize& size) { m_size = size; }
    void setAnchorRect(const QRect& anchorRect) { m_anchorRect = anchorRect; }
    void setAnchor(uint32_t anchor);
    void setGravity(uint32_t gravity);
    void setAdjustment(uint32_t adjustment) { m_adjustment = adjustment; }
    void setOffset(const QPoint& offset) { m_offset = offset; }

    QSize size() const { return m_size; }
    QRect anchorRect() const { return m_anchorRect; }

private:
    QSize m_size;
    QRect m_anchorRect;

    Direction m_anchorX = kNeutral;
    Direction m_anchorY = kNeutral;

    Direction m_gravityX = kNeutral;
    Direction m_gravityY = kNeutral;

    // Which modifications to the position/size are allowed; see
    // xdg_positioner.set_constraint_adjustment.
    uint32_t m_adjustment = 0;

    // An absolute translation, unaffected by flipping, sliding or resizing.
    QPoint m_offset;
};

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_PROTOCOLS_XDG_POSITIONER_H_
