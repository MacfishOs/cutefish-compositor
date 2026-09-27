// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_PROTOCOLS_XDG_DIALOG_H_
#define CUTEFISH_WAYLAND_PROTOCOLS_XDG_DIALOG_H_

#include <wayland-server-core.h>

#include <cstdint>

namespace cutefish::wayland {

constexpr uint32_t kXdgWmDialogVersion = 1;

// xdg_wm_dialog_v1. A modal xdg_dialog_v1 makes its toplevel window-modal on
// the toplevel's xdg_toplevel.set_parent.
wl_global *createXdgDialogGlobal(wl_display *display);

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_PROTOCOLS_XDG_DIALOG_H_
