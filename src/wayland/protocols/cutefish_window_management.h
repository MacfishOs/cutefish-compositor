// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_PROTOCOLS_CUTEFISH_WINDOW_MANAGEMENT_H_
#define CUTEFISH_WAYLAND_PROTOCOLS_CUTEFISH_WINDOW_MANAGEMENT_H_

#include <wayland-server-core.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <QObject>
#include <QPoint>

#include "compositor/surface_observer.h"
#include "wm/desks/desks_controller.h"
#include "wm/window_observer.h"

namespace cutefish::wm {
class Window;
class WindowManager;
}

namespace cutefish::wayland {

class PrivilegedClients;
class WaylandCutefishWindow;

constexpr uint32_t kCutefishWindowManagementVersion = 1;

// cutefish_window_management_v1: the task list the dock, the status bar and the
// launcher are built on.
//
// One manager resource at a time in practice - there is one trusted shell - but
// nothing here assumes it: each bound manager keeps its own set of
// cutefish_window_v1 objects, so a shell restart that rebinds simply gets a
// fresh enumeration while the old resources are torn down with the old client.
//
// Only normal toplevel windows are announced. The shell's own surfaces
// are private shell windows and must never appear in its own task list.
class WaylandCutefishWindowManagement : public QObject, public wm::WindowObserver,
                                        public compositor::SurfaceObserver,
                                        public wm::DesksObserver
{
    Q_OBJECT

public:
    WaylandCutefishWindowManagement(wm::WindowManager* windowManager,
                                    PrivilegedClients* privileged, QObject* parent = nullptr);
    ~WaylandCutefishWindowManagement() override;

    wm::WindowManager* windowManager() const { return m_windowManager; }
    PrivilegedClients* privileged() const { return m_privileged; }

    // Answers cutefish_window_v1.create_capture_source: creates the
    // ext_image_capture_source_v1 `id` for `window`, null once the window is
    // gone. Image capture installs it; without one the source only stops.
    using CaptureSourceExporter = std::function<void(wl_client*, uint32_t id, wm::Window* window)>;
    void setCaptureSourceExporter(CaptureSourceExporter exporter);
    const CaptureSourceExporter& captureSourceExporter() const { return m_captureSourceExporter; }

    // Announces every existing application window to a newly bound manager,
    // followed by the active window and initial_state_done.
    void sendInitialState(wl_resource* manager);

    void addManagerResource(wl_resource* manager);
    void removeManagerResource(wl_resource* manager);

    // Answers pick: what the capture interface points at.
    void pick(wl_resource* manager, uint32_t serial, const QPoint& position);

    void onSurfaceWindowHintsChanged(compositor::Surface* surface) override;
    void onSurfaceMenuChanged(compositor::Surface* surface) override;
    void onSurfaceDestroying(compositor::Surface* surface) override;

    // --- wm::WindowObserver ---

    void onWindowTitleChanged(wm::Window* window) override;
    void onWindowApplicationIdChanged(wm::Window* window) override;
    void onWindowBoundsChanged(wm::Window* window, const QRect& oldBounds) override;
    void onWindowStateTypeChanged(wm::Window* window, wm::WindowStateType oldType) override;
    void onWindowActivationChanged(wm::Window* window, bool active) override;
    void onWindowTransientParentChanged(wm::Window* window, wm::Window* oldParent) override;
    void onWindowResponsiveChanged(wm::Window* window) override;
    void onWindowHiddenChanged(wm::Window* window) override;
    void onWindowMinimizedSnapshotChanged(wm::Window* window) override;
    void onWindowMinimizeLanded(wm::Window* window) override;
    void onWindowDestroying(wm::Window* window) override;

    // --- wm::DesksObserver ---
    void onDeskAdded(const wm::Desk* desk) override;
    void onDeskRemoved(const wm::Desk* desk) override;
    void onDeskActivationChanged(const wm::Desk* activated, const wm::Desk* deactivated) override;
    void onDeskNameChanged(const wm::Desk* desk) override;
    void onWindowDeskChanged(wm::Window* window) override;

private:
    void onWindowAdded(wm::Window* window);
    void announceWindow(wm::Window* window);
    void closeWindowResources(wm::Window* window);
    void onWindowRemoved(wm::Window* window);
    void onActiveWindowChanged(wm::Window* window);

    // Every cutefish_window_v1 resource for `window`, across all bound
    // managers.
    std::vector<wl_resource*> resourcesFor(wm::Window* window) const;
    void sendState(wl_resource* resource, wm::Window* window);
    void sendParent(wl_resource* resource, wm::Window* window);
    void sendDesk(wl_resource* resource, wm::Window* window);
    void sendDesks(wl_resource* manager);
    // Sends the minimized snapshot, without done, when the window has one.
    void sendThumbnail(wl_resource* resource, wm::Window* window);

    wm::WindowManager* m_windowManager = nullptr;
    PrivilegedClients* m_privileged = nullptr;
    CaptureSourceExporter m_captureSourceExporter;
    std::vector<wl_resource*> m_managers;
    std::set<wm::Window*> m_observedWindows;
    std::vector<quint64> m_applicationHistory;
    quint64 m_activeWindowId = 0;
    // Window -> its per-manager resources. The window pointer is only ever a
    // key: it is erased before the window is destroyed.
    std::map<wm::Window*, std::vector<wl_resource*>> m_windowResources;
};

// `management` must outlive the global.
wl_global* createCutefishWindowManagementGlobal(wl_display* display,
                                                WaylandCutefishWindowManagement* management);

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_PROTOCOLS_CUTEFISH_WINDOW_MANAGEMENT_H_
