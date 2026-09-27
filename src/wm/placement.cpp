// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/window_positioning_utils.cc
//           chromium/src/ui/views/widget/native_widget_aura.cc (CenterWindow)
//           chromium/src/ui/gfx/geometry/rect.cc (AdjustToFit)
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: gfx::Rect becomes QRect, with exclusive edges kept.

#include "wm/placement.h"

#include <algorithm>

namespace cutefish::wm {

namespace {

void adjustAlongAxis(int areaOrigin, int areaSize, int *origin, int *size)
{
    *size = std::min(areaSize, *size);
    if (*origin < areaOrigin)
        *origin = areaOrigin;
    else
        *origin = std::min(areaOrigin + areaSize, *origin + *size) - *size;
}

} // namespace

void adjustBoundsSmallerThan(const QSize &maxSize, QRect *bounds)
{
    bounds->setWidth(std::min(bounds->width(), maxSize.width()));
    bounds->setHeight(std::min(bounds->height(), maxSize.height()));
}

void adjustBoundsToEnsureMinimumVisibility(const QRect &visibleArea, QRect *bounds,
                                           const QRect &grabArea)
{
    if (visibleArea.isEmpty())
        return;

    // A fixed strip rather than Ash's 30% of the window, so large windows can
    // still be pushed to the screen edge.
    int minWidth = kMinimumOnScreenArea;
    int minHeight = kMinimumOnScreenArea;

    adjustBoundsSmallerThan(visibleArea.size(), bounds);

    minWidth = std::min(minWidth, visibleArea.width());
    minHeight = std::min(minHeight, visibleArea.height());

    // QRect::right() is inclusive; this code works in exclusive edges
    // throughout, as Ash's gfx::Rect does.
    const int areaLeft = visibleArea.x();
    const int areaTop = visibleArea.y();
    const int areaRight = visibleArea.x() + visibleArea.width();
    const int areaBottom = visibleArea.y() + visibleArea.height();
    // The dock covers the bottom of the visible area; a strip pushed under it
    // has nothing left to grab, so the lower limit is the work area's.
    const int grabBottom = grabArea.isEmpty()
            ? areaBottom
            : std::min(areaBottom, grabArea.y() + grabArea.height());

    if (bounds->x() + bounds->width() < areaLeft + minWidth)
        bounds->moveLeft(areaLeft + std::min(bounds->width(), minWidth) - bounds->width());
    else if (bounds->x() > areaRight - minWidth)
        bounds->moveLeft(areaRight - std::min(bounds->width(), minWidth));

    if (bounds->y() + bounds->height() < areaTop + minHeight)
        bounds->moveTop(areaTop + std::min(bounds->height(), minHeight) - bounds->height());
    else if (bounds->y() > grabBottom - minHeight)
        bounds->moveTop(grabBottom - std::min(bounds->height(), minHeight));

    if (bounds->y() < areaTop)
        bounds->moveTop(areaTop);
}

void adjustBoundsToFit(const QRect &area, QRect *bounds)
{
    int x = bounds->x();
    int y = bounds->y();
    int width = bounds->width();
    int height = bounds->height();
    adjustAlongAxis(area.x(), area.width(), &x, &width);
    adjustAlongAxis(area.y(), area.height(), &y, &height);
    *bounds = QRect(x, y, width, height);
}

QRect Placement::place(const QSize &requestedSize, const QRect &workArea)
{
    QRect bounds(workArea.topLeft(), requestedSize);
    if (bounds.width() <= 0)
        bounds.setWidth(1);
    if (bounds.height() <= 0)
        bounds.setHeight(1);

    if (!workArea.isEmpty()) {
        adjustBoundsSmallerThan(workArea.size(), &bounds);

        bounds.moveTopLeft(workArea.topLeft()
                           + QPoint((workArea.width() - bounds.width()) / 2,
                                    (workArea.height() - bounds.height()) / 2));
    }

    return bounds;
}

QRect Placement::placeTransient(const QSize &requestedSize, const QRect &parentBounds,
                                int parentTopInset, const QRect &workArea)
{
    const QSize size = requestedSize.expandedTo(QSize(1, 1));

    QRect area = workArea;
    if (parentBounds.height() >= size.height() || parentBounds.width() >= size.width())
        area = parentBounds.adjusted(0, parentTopInset, 0, 0);

    QRect bounds(area.x() + (area.width() - size.width()) / 2,
                 area.y() + (area.height() - size.height()) / 2, size.width(), size.height());

    // Never larger than the work area, or its caption could end up out of reach.
    if (!workArea.isEmpty())
        adjustBoundsToFit(workArea, &bounds);
    return bounds;
}

} // namespace cutefish::wm
