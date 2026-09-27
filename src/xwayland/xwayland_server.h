// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include <wayland-server-core.h>

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include "surface_association.h"

class QSocketNotifier;

namespace cutefish::wayland {
class PrivilegedClients;
}

namespace cutefish::wm {
class WindowManager;
}

namespace cutefish::xwayland {

class XwaylandKeyboardGrab;
class XwaylandShell;
class Xwm;

class XwaylandServer : public QObject
{
public:
    struct Options {
        QString executable = QStringLiteral("/usr/bin/Xwayland");
        QString readinessFile;
        bool software = false;
        int timeoutMs = 10000;
        // Restarts after a crash allowed within one minute; beyond that
        // Xwayland is given up for the session.
        int restartLimit = 3;
        // How long an X11 client has to answer _NET_WM_PING; xdg_wm_base's
        // timeout by default.
        int pingTimeoutMs = 5000;
        // Top-level X11 windows tracked at once; the client holding the most
        // is disconnected beyond that.
        size_t windowLimit = SurfaceAssociation::limit;
        // Publish the display at once, but start Xwayland only when the first
        // X11 client connects to it.
        bool lazy = false;
    };

    XwaylandServer(wl_display*, wayland::PrivilegedClients*, wm::WindowManager*);
    ~XwaylandServer() override;
    bool start(const Options& options);
    void stop();

    // The output scale Xwayland renders at. X11 clients get device-resolution
    // coordinates and Xft.dpi, so they draw sharp instead of being upscaled.
    void setScale(double scale);

    // Xwayland is running and managed. A lazy server has published its display
    // before this.
    bool isReady() const { return m_ready; }

    QString display() const { return m_displayName; }

    QString authority() const { return m_authority; }

    QString error() const { return m_error; }

    qint64 processId() const { return m_process.processId(); }

    // The display is published to the session. For a lazy server that is
    // before Xwayland runs; started follows once it does.
    std::function<void()> ready;
    std::function<void()> started;
    std::function<void(const QString&)> failure;

private:
    bool launch();
    bool openSockets();
    void closeSockets();
    void arm();
    bool writeReadiness();
    void readDisplay();
    void fail(const QString& error);
    void deferFailure(const QString& error);
    void publish();
    // Ends the running server. `keepSession` keeps what the session was given
    // - display, Xauthority and readiness file - for a restart to reuse.
    void teardown(bool keepSession);
    bool mayRestart();
    void applyScale();
    wl_display* m_display;
    wayland::PrivilegedClients* m_privileged;
    wm::WindowManager* m_manager;
    wl_client* m_client = nullptr;

    struct Listener {
        wl_listener listener {};
        XwaylandServer* owner;
    } m_destroy;

    QProcess m_process;
    QTimer m_timeout;
    Options m_options;
    std::deque<qint64> m_restarts;
    // A lazy server's listening sockets and the lock file that reserves them.
    std::vector<int> m_listenFds;
    std::vector<std::unique_ptr<QSocketNotifier>> m_listenNotifiers;
    QString m_lockFile, m_socketFile;
    double m_scale = 1;
    std::unique_ptr<QTemporaryDir> m_runtime;
    std::unique_ptr<SurfaceAssociation> m_association;
    std::unique_ptr<XwaylandShell> m_shell;
    std::unique_ptr<XwaylandKeyboardGrab> m_keyboardGrab;
    std::unique_ptr<Xwm> m_xwm;
    QSocketNotifier* m_readNotifier = nullptr;
    int m_readFd = -1;
    QByteArray m_readiness;
    QString m_displayName, m_authority, m_error, m_readinessFile;
    uint64_t m_epoch = 0;
    bool m_ready = false, m_stopping = false, m_xwmReady = false, m_published = false;
};

} // namespace cutefish::xwayland
