// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <vector>
#include <wayland-server-core.h>

#include "surface_association.h"

namespace cutefish::wayland {
class PrivilegedClients;
class Server;
} // namespace cutefish::wayland

namespace cutefish::xwayland {

class XwaylandShell
{
public:
    XwaylandShell(wl_display* display, wayland::PrivilegedClients* privileged, wl_client* client,
                  SurfaceAssociation* association);
    ~XwaylandShell();

    void revokeClient();

    bool isBound() const { return m_bound; }

    std::function<void()> bound;

private:
    struct Role;
    static void bind(wl_client*, void*, uint32_t, uint32_t);
    static void getSurface(wl_client*, wl_resource*, uint32_t, wl_resource*);
    wl_global* m_global = nullptr;
    wayland::Server* m_server = nullptr;
    bool m_bound = false;
    wayland::PrivilegedClients* m_privileged;
    wl_client* m_client;
    SurfaceAssociation* m_association;
    std::vector<std::unique_ptr<Role>> m_roles;
};

} // namespace cutefish::xwayland
