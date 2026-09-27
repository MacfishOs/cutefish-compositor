// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_PROTOCOLS_CUTEFISH_SHELL_H_
#define CUTEFISH_WAYLAND_PROTOCOLS_CUTEFISH_SHELL_H_

#include <wayland-server-core.h>

#include <cstdint>

namespace cutefish::wm {
class WindowManager;
}

namespace cutefish::wayland {

class PrivilegedClients;

constexpr uint32_t kCutefishShellVersion = 1;

// The per-display state the cutefish_shell_v1 global hands to its resources.
//
// cutefish_shell_v1 is the private extension the trusted shell uses to say what
// its ordinary xdg surfaces are for. Everything it carries is adapted into
// wm:: concepts; no wl_resource crosses into the window manager.
class WaylandCutefishShell
{
public:
    WaylandCutefishShell(wm::WindowManager* windowManager, PrivilegedClients* privileged);
    WaylandCutefishShell(const WaylandCutefishShell&) = delete;
    WaylandCutefishShell& operator=(const WaylandCutefishShell&) = delete;

    wm::WindowManager* windowManager() const { return m_windowManager; }
    PrivilegedClients* privileged() const { return m_privileged; }

private:
    wm::WindowManager* m_windowManager = nullptr;
    PrivilegedClients* m_privileged = nullptr;
};

// `shell` must outlive the global. Registering the returned global with the
// privileged client registry is what hides the interface from ordinary clients.
wl_global* createCutefishShellGlobal(wl_display* display, WaylandCutefishShell* shell);

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_PROTOCOLS_CUTEFISH_SHELL_H_
