// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QObject>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "wm/window_observer.h"

struct wl_client;
struct wl_display;
struct wl_global;
struct wl_resource;

namespace cutefish::compositor {
class CaptureSource;
class Compositor;
}

namespace cutefish::wm {
class Window;
class WindowManager;
}

namespace cutefish::wayland {

// ext_foreign_toplevel_list_v1 for capture clients: every ordinary application
// window as a handle carrying title, app id and a stable identifier, and, for
// the capture source manager, the window's own CaptureSource. The shell keeps
// using its private window-management protocol; this global is only announced
// to clients allowed to capture.
class ForeignToplevelList : public QObject, private wm::WindowObserver
{
public:
    using Authorize = std::function<bool(const wl_client *)>;

    ForeignToplevelList(wl_display *display, wm::WindowManager *manager,
                        compositor::Compositor *compositor, Authorize authorize);
    ~ForeignToplevelList() override;

    // Releases the global and closes every handle; before the display goes.
    void shutdown();

    wl_global *global() const { return m_global; }

    // The capture source behind a handle resource, or null for a handle of
    // another client's list or a closed window.
    std::shared_ptr<compositor::CaptureSource> sourceFor(wl_resource *handle) const;

    // List resources bound and not yet destroyed.
    int boundListCount() const { return int(m_clients.size()); }

private:
    struct Client;
    struct Entry;
    friend struct ForeignToplevelListEvents;

    static bool listable(const wm::Window *window);
    void add(wm::Window *window);
    void remove(wm::Window *window);
    void announce(Client &client, Entry &entry);
    void updateGeometry(Entry &entry);
    void updateAllGeometry();

    void onWindowBoundsChanged(wm::Window *window, const QRect &oldBounds) override;
    void onWindowApplicationIdChanged(wm::Window *window) override;
    void onWindowTitleChanged(wm::Window *window) override;

    wl_display *m_display;
    wm::WindowManager *m_manager;
    compositor::Compositor *m_compositor;
    Authorize m_authorize;
    wl_global *m_global = nullptr;
    std::vector<std::unique_ptr<Client>> m_clients;
    std::map<wm::Window *, std::unique_ptr<Entry>> m_entries;
};

} // namespace cutefish::wayland
