// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_PROTOCOLS_XDG_FOREIGN_H_
#define CUTEFISH_WAYLAND_PROTOCOLS_XDG_FOREIGN_H_

#include <wayland-server-core.h>

#include <cstdint>

namespace cutefish::wayland {

constexpr uint32_t kXdgExporterV2Version = 1;
constexpr uint32_t kXdgImporterV2Version = 1;

// zxdg_exporter_v2 / zxdg_importer_v2. One client exports a toplevel and passes
// the handle to another out of band; the importer makes its own toplevel a
// child of it, with xdg_toplevel.set_parent semantics. Out-of-process dialogs
// (the file chooser portal) need this to parent themselves. Exo has no
// equivalent, so this follows the protocol rather than upstream.
wl_global *createXdgExporterV2Global(wl_display *display);
wl_global *createXdgImporterV2Global(wl_display *display);

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_PROTOCOLS_XDG_FOREIGN_H_
