// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_PROTOCOL_GLOBALS_H_
#define CUTEFISH_WAYLAND_PROTOCOL_GLOBALS_H_

#include <memory>

#include <QString>

struct wl_global;

namespace cutefish {

namespace compositor {
class Compositor;
}

namespace input {
class InputMethodSettings;
class Seat;
class InputManager;
} // namespace input

namespace scene {
class Node;
}

namespace wm {
class WindowManager;
}

namespace wayland {

class DataControlManager;
class ForeignToplevelList;
class IdleInhibition;
class ImageCaptureManager;
class InputMethodLauncher;
class InputMethodManager;
class PrivilegedClients;
class SecurityDelegate;
class Server;
class WaylandCutefishShell;
class WaylandCutefishWindowManagement;
class WaylandXdgActivation;
class WaylandXdgShell;
struct WaylandSeat;

// Every device-independent Wayland global this compositor registers, together
// with the state each global binds to.
//
// Upstream: chromium/src/components/exo/wayland/server.cc, Server::Initialize().
// Exo keeps this inside Server; here it is a separate type because
// cutefish-wayland is the base library the protocol implementations are built
// on, so the registration has to sit above them rather than inside it.
//
// Globals that depend on the graphics device - linux-dmabuf, drm-syncobj,
// presentation - are registered by whoever owns that device, not here.
class ProtocolGlobals
{
public:
    struct Dependencies {
        Server *server = nullptr;
        compositor::Compositor *compositor = nullptr;
        input::Seat *seat = nullptr;
        input::InputManager *inputManager = nullptr;
        wm::WindowManager *windowManager = nullptr;
        // Candidate popups stack here, above every window but below the lock
        // screen.
        scene::Node *inputMethodPopupLayer = nullptr;
        SecurityDelegate *security = nullptr;
        // The greeter and development instances must not take over the user's
        // input method.
        bool launchInputMethod = false;
        // Development override; empty means use the saved selection.
        QString inputMethodOverride;
    };

    explicit ProtocolGlobals(const Dependencies &dependencies);
    ProtocolGlobals(const ProtocolGlobals &) = delete;
    ProtocolGlobals &operator=(const ProtocolGlobals &) = delete;
    ~ProtocolGlobals();

    // Registers every global; call once the server is listening. False means the
    // compositor cannot serve clients, and the reason has already been logged.
    // The privileged registry filter is in place from construction, before the
    // socket exists.
    bool initialize();

    // Releases the globals. Must run after the clients are gone and before the
    // display is destroyed.
    void shutdown();

    // Spawns the input method once, with `x11Display` set when Xwayland is up so
    // it can serve X11 clients too. Later calls are ignored.
    void startInputMethod(const QString &x11Display, const QString &x11Authority);
    // Restarts a started input method with this X11 display: Xwayland started
    // on demand after it, or restarted after a crash.
    void setInputMethodX11Display(const QString &x11Display, const QString &x11Authority);

    // Keeps input-method popups on the text cursor when windows move.
    void sceneChanged();

    PrivilegedClients *privileged() { return m_privileged.get(); }

    IdleInhibition *idleInhibition() { return m_idleInhibition.get(); }

    ImageCaptureManager *imageCapture() { return m_imageCapture.get(); }

    InputMethodLauncher *inputMethodLauncher() { return m_inputMethodLauncher.get(); }

private:
    bool registerCoreGlobals();
    bool registerInputMethod();
    bool registerClipboard();
    bool registerCapture();
    bool registerPrivateShell();
    bool registerQtHardwareIntegration();

    const Dependencies m_deps;
    bool m_inputMethodStarted = false;

    // Declaration order is destruction order reversed, and it matters: this
    // mirrors the order these objects had as locals of main().
    std::unique_ptr<PrivilegedClients> m_privileged;
    std::unique_ptr<WaylandSeat> m_seat;
    std::unique_ptr<IdleInhibition> m_idleInhibition;
    std::unique_ptr<WaylandXdgActivation> m_xdgActivation;
    std::unique_ptr<WaylandXdgShell> m_xdgShell;
    std::unique_ptr<InputMethodManager> m_inputMethod;
    std::unique_ptr<InputMethodLauncher> m_inputMethodLauncher;
    std::unique_ptr<input::InputMethodSettings> m_inputMethodSettings;
    std::unique_ptr<DataControlManager> m_dataControl;
    std::unique_ptr<ImageCaptureManager> m_imageCapture;
    std::unique_ptr<ForeignToplevelList> m_toplevelList;
    std::unique_ptr<WaylandCutefishShell> m_cutefishShell;
    std::unique_ptr<WaylandCutefishWindowManagement> m_windowManagement;
    wl_global *m_qtHardwareIntegration = nullptr;
};

} // namespace wayland
} // namespace cutefish

#endif // CUTEFISH_WAYLAND_PROTOCOL_GLOBALS_H_
