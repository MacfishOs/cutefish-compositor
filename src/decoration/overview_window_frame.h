// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_DECORATION_OVERVIEW_WINDOW_FRAME_H_
#define CUTEFISH_DECORATION_OVERVIEW_WINDOW_FRAME_H_

#include "scene/overview_node.h"

namespace cutefish::decoration {

class Theme;

// Every overview window has the corners, the shadow and the outline of a
// window in its normal state, the shadow of the active one.
scene::OverviewWindowFrame overviewWindowFrame(const Theme *theme);

} // namespace cutefish::decoration

#endif // CUTEFISH_DECORATION_OVERVIEW_WINDOW_FRAME_H_
