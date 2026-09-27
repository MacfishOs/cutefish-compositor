// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modelled on chromium/src/ash/wm/workspace/workspace_window_resizer.cc
// (revision 7acd40ddefff49c9d36144999aafdbb674dbd07a): GetNonSnapWorkArea,
// GetActiveDragAreaForSnapAndMaximize and GetSnapType, plus the snapped bounds
// ash::GetSnappedWindowBounds computes.
//
// Only Ash's landscape layout is ported: left and right snap to half the work
// area, the top maximizes. Ash's portrait orientation, where the top and the
// bottom snap instead, has no Cutefish equivalent and is left out. The snap
// ratio is always a half; Ash carries a per-window ratio for its resizable
// split view.

#ifndef CUTEFISH_WM_WINDOW_SNAP_H_
#define CUTEFISH_WM_WINDOW_SNAP_H_

#include <QPointF>
#include <QRect>
#include <QSize>

#include "wm/types.h"

namespace cutefish::wm {

// What the edge the pointer is dragging against asks for.
enum class SnapType {
    None,
    Primary, // Left half.
    Secondary, // Right half.
    Maximize,
};

// The area a drag can move through without asking for anything. Outside it, on
// one of the screen edges, is where a snap or a maximize is triggered. The
// tolerance is only added on the edges where the work area reaches the output:
// past a dock the pointer has to cross the dock first.
QRect nonSnapArea(const QRect &workArea, const QRect &outputBounds);

// `location` is in output coordinates. Returns what a drag ending there would
// ask for, before any window constraint is taken into account.
SnapType snapTypeAt(const QPointF &location, const QRect &workArea, const QRect &outputBounds);

// Empty for SnapType::None.
QRect snapBounds(SnapType type, const QRect &workArea);

WindowStateType snapStateType(SnapType type);

// A window narrower than half the work area cannot be snapped into it.
bool canSnapSize(const QSize &minimumOuterSize, const QRect &workArea);

// Whether `bounds` still look snapped: full work-area height, against one of
// its vertical edges. Ash's WorkspaceWindowResizer::AreBoundsValidSnappedBounds.
bool areBoundsValidSnappedBounds(WindowStateType type, const QRect &bounds, const QRect &workArea);

const char *toString(SnapType type);

} // namespace cutefish::wm

#endif // CUTEFISH_WM_WINDOW_SNAP_H_
