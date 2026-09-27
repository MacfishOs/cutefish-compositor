// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <functional>
#include <wayland-server-core.h>

namespace cutefish::wayland {

struct WaylandSeat;

class DataControlManager
{
public:
    using Authorize = std::function<bool(const wl_client*)>;
    DataControlManager(wl_display* display, WaylandSeat* seat, Authorize authorize);
    ~DataControlManager();
    void shutdown();
    DataControlManager(const DataControlManager&) = delete;
    DataControlManager& operator=(const DataControlManager&) = delete;

    wl_global* extGlobal() const { return m_ext; }

    wl_global* wlrGlobal() const { return m_wlr; }

    bool authorized(const wl_client* client) const { return m_authorize && m_authorize(client); }

    WaylandSeat* seat() const { return m_seat; }

private:
    WaylandSeat* m_seat;
    Authorize m_authorize;
    wl_global* m_ext;
    wl_global* m_wlr;
};

} // namespace cutefish::wayland
