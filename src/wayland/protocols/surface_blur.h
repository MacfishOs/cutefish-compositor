// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_SURFACE_BLUR_H_
#define CUTEFISH_WAYLAND_SURFACE_BLUR_H_

#include <wayland-server-core.h>

namespace cutefish::ui {
class Path;
}

namespace cutefish::wayland {

wl_global* createSurfaceBlurGlobal(wl_display* display);

// An array of cutefish_surface_blur_v1.path_command values and wl_fixed
// points as a path; false for a malformed one or one past 4096 values.
// cutefish_window_hints_v1 shares the encoding.
bool pathFromArray(const wl_array* array, ui::Path* result);

} // namespace cutefish::wayland

#endif
