// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modelled on chromium/src/ash/wm/window_resizer.* and
// default_window_resizer.* (revision 7acd40ddefff49c9d36144999aafdbb674dbd07a).
//
// The delta/origin/size math is Ash's, including the trick of clamping the
// delta - not just the resulting size - so that the origin computation keeps
// the opposite edge fixed once a size limit is reached, and the restore-bounds
// branch of GetOriginForDrag/GetSizeForDrag that pulls a tiled window out from
// under the pointer. Chromium's HT* constants are replaced by ResizeEdge, and
// aspect ratio, touch resize and multi-display handling are dropped. Ash also
// shifts the origin so the pointer never lands on a caption button of the
// restored frame; that needs the frame's button layout and is left out.

#ifndef CUTEFISH_WM_WINDOW_RESIZER_H_
#define CUTEFISH_WM_WINDOW_RESIZER_H_

#include <QPointF>
#include <QRect>

#include "wm/types.h"

namespace cutefish::wm {

class Window;

class WindowResizer
{
public:
    // `edge` of ResizeEdge::None means a move rather than a resize.
    // `restoreBounds` is the size and the aspect the window is being dragged
    // back to, set only when a move is pulling a maximized or snapped window
    // out of its state; empty for every other drag. It also arms the small
    // threshold a tiled window has to be dragged past before it moves at all.
    // `visibleArea` bounds how far off screen it may go; empty means `workArea`.
    WindowResizer(Window *window, const QPointF &startLocation, ResizeEdge edge,
                  const QRect &workArea, const QRect &restoreBounds = QRect(),
                  const QRect &visibleArea = QRect());

    Window *window() const { return m_window; }

    ResizeEdge edge() const { return m_edge; }

    bool isResize() const { return m_edge != ResizeEdge::None; }

    QRect initialBounds() const { return m_initialBounds; }

    QRect restoreBounds() const { return m_restoreBounds; }

    // False until the drag has passed the threshold and actually changed the
    // window. Ash's did_move_or_resize_.
    bool hasMovedOrResized() const { return m_movedOrResized; }

    // Pure function of the drag start and `location`; does not touch the
    // window. Exposed so tests can check the math without a scene.
    QRect calculateBoundsForDrag(const QPointF &location) const;

    void drag(const QPointF &location);
    void completeDrag();
    // Puts the window back where the drag started.
    void revertDrag();

private:
    int widthForDrag(int *deltaX) const;
    int heightForDrag(int *deltaY) const;

    Window *m_window = nullptr;
    ResizeEdge m_edge = ResizeEdge::None;
    QPointF m_initialLocation;
    QRect m_initialBounds;
    QRect m_restoreBounds;
    QRect m_workArea;
    QRect m_visibleArea;
    bool m_movedOrResized = false;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_WINDOW_RESIZER_H_
