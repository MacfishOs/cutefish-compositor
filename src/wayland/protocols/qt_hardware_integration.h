// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QString>

class QIODevice;

struct wl_client;
struct wl_display;
struct wl_global;

namespace cutefish::wayland {

// Points Qt clients at qt-plugins' cutefish-egl client buffer integration,
// which commits each viewport destination together with its buffer.
wl_global* createQtHardwareIntegrationGlobal(wl_display* display);

// Qt Quick aborts when the integration it is pointed at cannot be loaded, so
// only processes running the host's Qt 6 with the plugin installed may see
// the global: not Qt 5, Flatpak runtimes or bundled Qt builds. The plugin uses
// Qt's private ABI, so its recorded Qt versions must be the host's.
// `sessionClient` is a connection the session created, whose peer credentials
// name the session rather than the client.
bool canLoadCutefishEgl(const wl_client* client, bool sessionClient);

// The QtWaylandClient file in a /proc/<pid>/maps listing, or an empty string.
// Exposed for testing.
QString findMappedQtWaylandClient(QIODevice& maps);

} // namespace cutefish::wayland
