// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/window_positioning_utils.{h,cc}
//           chromium/src/ui/views/widget/native_widget_aura.cc (CenterWindow)
//           chromium/src/ui/gfx/geometry/rect.cc (AdjustToFit)
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: gfx::Rect becomes QRect, with exclusive edges kept.

#ifndef CUTEFISH_WM_PLACEMENT_H_
#define CUTEFISH_WM_PLACEMENT_H_

#include <QRect>
#include <QSize>

namespace cutefish::wm {

// A window must keep at least this much of itself reachable inside the work
// area, so that it can always be grabbed again.
inline constexpr int kMinimumOnScreenArea = 30;

// Shrinks `bounds` so neither dimension exceeds `maxSize`. The origin is kept.
void adjustBoundsSmallerThan(const QSize &maxSize, QRect *bounds);

// Slides `bounds` back until a usable strip of it overlaps `visibleArea`, and
// never lets its top escape above the area - dragging a window off the top edge
// would put its titlebar-equivalent region out of reach. `grabArea` is where
// that strip has to end up when it is pushed down: the work area, so the top of
// a window can slide under the dock only as far as it stays reachable beside
// it. Empty means `visibleArea`.
void adjustBoundsToEnsureMinimumVisibility(const QRect &visibleArea, QRect *bounds,
                                           const QRect &grabArea = QRect());

// Shrinks `bounds` to fit inside `area`, then moves it fully inside.
void adjustBoundsToFit(const QRect &area, QRect *bounds);

// Where new windows go. One instance per WindowManager.
class Placement
{
public:
    // Centers the window in the work area, shrinking it to fit when necessary.
    QRect place(const QSize &requestedSize, const QRect &workArea);

    // Centers a transient window over its parent's client area, as long as the
    // parent is at least as large in one dimension; otherwise over the work
    // area. Always kept inside the work area.
    QRect placeTransient(const QSize &requestedSize, const QRect &parentBounds, int parentTopInset,
                         const QRect &workArea);
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_PLACEMENT_H_
