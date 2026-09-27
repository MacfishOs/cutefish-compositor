// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_PRIVILEGED_CLIENT_H_
#define CUTEFISH_WAYLAND_PRIVILEGED_CLIENT_H_

#include <functional>
#include <map>
#include <vector>
#include <wayland-server-core.h>

namespace cutefish::wayland {

// The privilege boundary for the private shell protocols.
//
// Exactly one wl_client at a time is the trusted shell. It is not recognised by
// uid, by pid or by anything else it could claim about itself: the session
// hands the compositor one end of a socketpair it created, the compositor turns
// that fd into a wl_client with wl_client_create(), and *that* client object is
// the trusted one. Nothing that connects to the public socket can become it.
//
// A global registered here is invisible to every other client - the filter runs
// before wl_registry.global is sent - and its bind handler must check
// isTrusted() as well, so a stale or forged bind cannot slip through.
class PrivilegedClients
{
public:
    explicit PrivilegedClients(wl_display *display);
    PrivilegedClients(const PrivilegedClients &) = delete;
    PrivilegedClients &operator=(const PrivilegedClients &) = delete;
    ~PrivilegedClients();
    void shutdown();

    // Takes ownership of `fd` and makes the resulting connection the trusted
    // shell, replacing any previous one. Returns null and closes `fd` on
    // failure.
    wl_client *adoptShellConnection(int fd);

    wl_client *shellClient() const { return m_shell.client; }

    bool isTrusted(const wl_client *client) const { return client && client == m_shell.client; }

    // Globals only the trusted shell may see or bind.
    void addPrivateGlobal(wl_global *global);
    void addRestrictedGlobal(wl_global *global, std::function<bool(const wl_client *)> authorize);
    void removeRestrictedGlobal(wl_global *global);
    void removePrivateGlobal(wl_global *global);

    // Runs after the trusted client has gone and the stored client is already
    // null, so a handler is free to tear its protocol state down.
    void setShellDisconnectedCallback(std::function<void()> callback)
    {
        m_shell.disconnected = std::move(callback);
    }

private:
    struct TrustedConnection;

    // Standard layout, so libwayland's wl_listener* can be cast back to it.
    struct DestroyListener {
        wl_listener listener;
        TrustedConnection *connection;
    };

    // One adopted connection: the client, its destroy listener, and what to run
    // when it goes away.
    struct TrustedConnection {
        wl_client *client = nullptr;
        std::function<void()> disconnected;
        const char *label = "";
        DestroyListener destroy {};
        bool listening = false;
    };

    static bool globalFilter(const wl_client *client, const wl_global *global, void *data);
    static void onConnectionDestroyed(wl_listener *listener, void *data);

    wl_client *adopt(TrustedConnection &connection, int fd);
    static void forget(TrustedConnection &connection);

    wl_display *m_display = nullptr;
    TrustedConnection m_shell {nullptr, {}, "shell", {}, false};
    std::vector<wl_global *> m_privateGlobals;
    std::map<const wl_global *, std::function<bool(const wl_client *)>> m_restrictedGlobals;
};

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_PRIVILEGED_CLIENT_H_
