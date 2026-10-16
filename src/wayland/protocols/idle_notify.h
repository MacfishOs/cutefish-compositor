// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

struct wl_display;
struct wl_global;

namespace cutefish::wayland {
class IdleInhibition;
wl_global *createIdleNotifierGlobal(wl_display *display, IdleInhibition *inhibition);
} // namespace cutefish::wayland
