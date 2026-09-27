// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/security_delegate.h"

#include <unistd.h>

#include <wayland-server-core.h>

#include <QFileInfo>

namespace cutefish::wayland {

namespace {

QStringList canonicalPaths(const QStringList &paths)
{
    QStringList result;
    for (const auto &path : paths) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty())
            result.append(canonical);
    }
    return result;
}

// /proc/<pid> itself belongs to the process owner; its exe link would resolve
// to a root-owned file under /usr.
bool runsOneOf(pid_t pid, const QStringList &executables)
{
    return pid > 0 && QFileInfo(QStringLiteral("/proc/%1").arg(pid)).ownerId() == getuid()
            && executables.contains(
                    QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).canonicalFilePath());
}

bool runsOneOf(const wl_client *client, const QStringList &executables)
{
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    wl_client_get_credentials(const_cast<wl_client *>(client), &pid, &uid, &gid);
    return uid == getuid() && runsOneOf(pid, executables);
}

} // namespace

ExecutableSecurityDelegate::ExecutableSecurityDelegate(const QStringList &clipboardClients,
                                                       const QStringList &captureClients,
                                                       const QStringList &systemDialogClients,
                                                       bool captureEnabled,
                                                       const QStringList &floatingClients)
    : m_clipboardClients(canonicalPaths(clipboardClients))
    , m_captureClients(canonicalPaths(captureClients))
    , m_systemDialogClients(canonicalPaths(systemDialogClients))
    , m_floatingClients(canonicalPaths(floatingClients))
    , m_captureEnabled(captureEnabled)
{
}

bool ExecutableSecurityDelegate::canManageClipboard(const wl_client *client) const
{
    return runsOneOf(client, m_clipboardClients);
}

bool ExecutableSecurityDelegate::canCaptureOutputs(const wl_client *client) const
{
    return m_captureEnabled && runsOneOf(client, m_captureClients);
}

bool ExecutableSecurityDelegate::showsSystemDialogs(qint64 pid) const
{
    return runsOneOf(static_cast<pid_t>(pid), m_systemDialogClients);
}

bool ExecutableSecurityDelegate::showsFloatingWindows(qint64 pid) const
{
    return runsOneOf(static_cast<pid_t>(pid), m_floatingClients);
}

} // namespace cutefish::wayland
