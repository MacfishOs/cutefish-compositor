// Copyright 2017 The ChromiumOS Authors
// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.sommelier file.
//
// Upstream: chromiumos/platform2/vm_tools/sommelier/{sommelier,sommelier-window}.cc
// Revision: aa8a57908bdcff297e229679dc3240b1b108f57e

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <xcb/xcb.h>

#include <QObject>
#include <QRect>
#include <QRegion>

#include "compositor/surface_observer.h"
#include "wm/window_observer.h"

class QSocketNotifier;

namespace cutefish::wm {
class WindowManager;
}

namespace cutefish::xwayland {

class SurfaceAssociation;
class SelectionBridge;
class DndBridge;

class Xwm : public QObject
{
public:
    Xwm(wm::WindowManager *manager, SurfaceAssociation *association);
    ~Xwm() override;
    void start(int fd);
    // X11 pixels per logical unit. Xwayland draws at device resolution, so
    // every coordinate crossing between X11 and the window manager is scaled.
    void setScale(double scale);
    // How long a client has to answer _NET_WM_PING.
    void setPingTimeout(int milliseconds) { m_pingTimeoutMs = milliseconds; }
    void setWindowLimit(size_t limit) { m_windowLimit = limit; }
    // Xwayland's own process, never reported as a window's owner nor killed.
    void setServerProcess(qint64 pid) { m_serverPid = pid; }
    std::function<void(bool)> initialized;
    std::function<void()> failed;

private:
    struct X11Window;

    struct Reply {
        unsigned sequence;
        std::function<void(void *)> callback;
    };

    void setup();
    void drain();
    void event(xcb_generic_event_t *event);
    // Reads `atoms`, or every property the window manager follows.
    void properties(X11Window &window, std::vector<xcb_atom_t> atoms = {});
    std::vector<xcb_atom_t> followedProperties();
    void shape(X11Window &window);
    void present(X11Window &window);
    void detach(X11Window &window);
    // Follows a new child of the root, unless its client holds too many windows.
    X11Window *adopt(uint32_t window, const QRect &xGeometry, bool overrideRedirect);
    // Stops following a window that is gone from the root.
    void forget(uint32_t window);
    void synchronize(X11Window &window);
    void roots();
    void focus(X11Window &window, bool active);
    void message(uint32_t window, xcb_atom_t protocol, xcb_timestamp_t time = XCB_CURRENT_TIME);
    void property(uint32_t window, const char *name, xcb_atom_t type,
                  const std::vector<uint32_t> &values);
    // The same, unless `written` shows the window already has `values`.
    void property(uint32_t window, const char *name, xcb_atom_t type,
                  std::vector<uint32_t> values, std::optional<std::vector<uint32_t>> &written);
    void reply(unsigned sequence, std::function<void(void *)> callback);
    xcb_atom_t atom(const char *name) const;
    QPoint fromX(const QPoint &point) const;
    QPoint toX(const QPoint &point) const;
    QSize sizeFromX(const QSize &size) const;
    QSize sizeToX(const QSize &size) const;
    QRegion regionFromX(const QRegion &region) const;
    void publishDpi();
    void ping(uint32_t window);
    bool requestSync(X11Window &window);
    void kill(uint32_t window);
    static uint32_t ownerPid(const X11Window &window);
    void applyHints(X11Window &window);
    // One _NET_WM_STATE client message applied to the state read from the
    // property, for a window that has no managed window yet.
    void applyStateRequest(X11Window &window, const std::array<uint32_t, 3> &request);
    wm::WindowManager *m_manager;
    SurfaceAssociation *m_association;
    xcb_connection_t *m_connection = nullptr;
    xcb_window_t m_root = 0;
    xcb_window_t m_support = 0;
    int m_fd = -1;
    int m_shutdownFd = -1;
    std::thread m_setupThread;
    QSocketNotifier *m_notifier = nullptr;
    std::map<std::string, xcb_atom_t> m_atoms;
    std::map<uint32_t, std::unique_ptr<X11Window>> m_windows;
    std::deque<Reply> m_replies;
    uint64_t m_generation = 0;
    uint64_t m_mapOrder = 0;
    bool m_ready = false;
    bool m_initializationFailed = false;
    xcb_void_cookie_t m_rootSubscription {};
    xcb_void_cookie_t m_redirection {};
    QRect m_lastOutput, m_lastWorkArea;
    double m_scale = 1;
    bool m_xsettings = false;
    uint32_t m_xsettingsSerial = 0;
    int m_pingTimeoutMs = 5000;
    size_t m_windowLimit = SIZE_MAX;
    // The client disconnected for holding too many windows, until the count
    // drops below the limit again.
    uint32_t m_disconnecting = 0;
    uint32_t m_pingSerial = 0;
    qint64 m_serverPid = 0;
    bool m_clientPids = false;
    bool m_draining = false;
    uint8_t m_xfixesEvent = 0;
    uint8_t m_shapeEvent = 0;
    uint8_t m_syncEvent = 0;
    std::unique_ptr<SelectionBridge> m_selection, m_primarySelection;
    std::unique_ptr<DndBridge> m_dnd;
};

} // namespace cutefish::xwayland
