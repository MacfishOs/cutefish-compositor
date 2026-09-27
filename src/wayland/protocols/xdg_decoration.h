// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/wayland/xdg_shell.cc
//           (WaylandXdgToplevelDecoration and the manager request table)
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: Exo keeps the negotiated mode inside the protocol
// object and pushes a SurfaceFrameType at the shell surface; here the mode
// belongs to wm::ShellSurface, so that the decision survives the protocol
// object and so that nothing about frames lives in a request handler. Exo also
// leaves unset_mode unimplemented and posts none of the protocol errors; both
// are implemented here.

#ifndef CUTEFISH_WAYLAND_PROTOCOLS_XDG_DECORATION_H_
#define CUTEFISH_WAYLAND_PROTOCOLS_XDG_DECORATION_H_

#include <wayland-server-core.h>

#include <cstdint>

namespace cutefish::wayland {

constexpr uint32_t kZxdgDecorationManagerVersion = 1;

wl_global* createXdgDecorationGlobal(wl_display* display);

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_PROTOCOLS_XDG_DECORATION_H_
