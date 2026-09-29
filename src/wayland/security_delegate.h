// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_SECURITY_DELEGATE_H_
#define CUTEFISH_WAYLAND_SECURITY_DELEGATE_H_

#include <QStringList>

struct wl_client;

namespace cutefish::wayland {

// What a client may do beyond the ordinary protocol surface.
//
// Upstream: chromium/src/components/exo/security_delegate.h, narrowed to the
// privileges Cutefish actually grants. Protocol code asks the delegate instead
// of carrying a policy of its own.
class SecurityDelegate
{
public:
    virtual ~SecurityDelegate() = default;

    virtual bool canManageClipboard(const wl_client *client) const = 0;
    virtual bool canCaptureOutputs(const wl_client *client) const = 0;
    // Whether the process's toplevels are the session's own dialogs, stacked
    // above every application window.
    virtual bool showsSystemDialogs(qint64 pid) const = 0;
    virtual bool showsFloatingWindows(qint64) const { return false; }
    // The computer-use service, which may ask the user to let an agent drive
    // the seat, and reads the screen while it does.
    virtual bool canControlComputer(const wl_client *) const { return false; }
};

// Grants a privilege when the connecting process runs one of a fixed set of
// executables. The pid comes from the kernel through the socket, so a client
// cannot claim an identity it does not have.
class ExecutableSecurityDelegate final : public SecurityDelegate
{
public:
    // Paths are canonicalized once; entries that do not resolve are dropped, so
    // a client can never match an empty path.
    ExecutableSecurityDelegate(const QStringList &clipboardClients,
                               const QStringList &captureClients,
                               const QStringList &systemDialogClients, bool captureEnabled,
                               const QStringList &floatingClients = {},
                               const QStringList &computerControlClients = {});

    bool canManageClipboard(const wl_client *client) const override;
    bool canCaptureOutputs(const wl_client *client) const override;
    bool showsSystemDialogs(qint64 pid) const override;
    bool showsFloatingWindows(qint64 pid) const override;
    bool canControlComputer(const wl_client *client) const override;

private:
    const QStringList m_clipboardClients;
    const QStringList m_captureClients;
    const QStringList m_systemDialogClients;
    const QStringList m_floatingClients;
    const QStringList m_computerControlClients;
    // Capture has nothing to read without a display, and computer control
    // nothing to show.
    const bool m_captureEnabled;
};

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_SECURITY_DELEGATE_H_
