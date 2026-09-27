// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once
#include <wayland-server-core.h>

namespace cutefish::wayland {
struct WaylandSeat;
wl_global *createPrimarySelectionGlobal(wl_display *display, WaylandSeat *seat);
} // namespace cutefish::wayland
