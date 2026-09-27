// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_WINDOW_HINTS_H
#define CUTEFISH_WAYLAND_WINDOW_HINTS_H

#include <wayland-server-core.h>

namespace cutefish::wayland {

wl_global *createWindowHintsGlobal(wl_display *display);

} // namespace cutefish::wayland

#endif
