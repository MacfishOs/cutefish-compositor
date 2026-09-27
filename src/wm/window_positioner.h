// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/window_positioner.{h,cc}
//           (GetBoundsAndShowStateForNewWindow, GetReferenceWindow)
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: the reference window is the application's own most
// recent window, and a new window cascades from it rather than taking its
// bounds, since a Wayland client has chosen its size before it maps. Ash's
// automatic side-by-side rearrangement is not ported.

#ifndef CUTEFISH_WM_WINDOW_POSITIONER_H_
#define CUTEFISH_WM_WINDOW_POSITIONER_H_

#include <QRect>
#include <QString>

#include "wm/types.h"

namespace cutefish::wm {

class Window;
class WindowManager;

namespace window_positioner {

// Where a new `window`, already centered in its work area, should go: offset
// down and right from its application's most recent window on the same output,
// or from that window's restore bounds while it is maximized. A cascade that
// would run past the work area restarts from its top-left corner.
QRect boundsForNewWindow(const Window *window);

// The state a new window of `applicationId` on `output` should be shown in,
// decided before it exists. A window that asked for nothing is maximized when
// its application's most recent window is.
WindowStateType showStateForNewWindow(WindowManager *manager, const QRect &output,
                                      const QString &applicationId, WindowStateType requested);

} // namespace window_positioner

} // namespace cutefish::wm

#endif // CUTEFISH_WM_WINDOW_POSITIONER_H_
