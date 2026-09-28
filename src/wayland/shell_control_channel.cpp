// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/shell_control_channel.h"

#include <QFile>
#include <QSocketNotifier>

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/logging.h"

namespace cutefish::wayland {

namespace {

// One control message, one fd. The payload byte exists only because a
// zero-length datagram would not carry the ancillary data.
int receiveFd(int socket)
{
    char payload = 0;
    iovec iov {};
    iov.iov_base = &payload;
    iov.iov_len = 1;

    union {
        char buffer[CMSG_SPACE(sizeof(int))];
        cmsghdr alignment;
    } control {};

    msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.buffer;
    message.msg_controllen = sizeof(control.buffer);

    const ssize_t received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    if (received <= 0)
        return received == 0 ? -1 : -2;

    // One descriptor is expected; every other one delivered still belongs to
    // this process and would leak if it were merely skipped.
    int fd = -1;
    for (cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
            continue;
        const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int received = -1;
            std::memcpy(&received, CMSG_DATA(header) + i * sizeof(int), sizeof(received));
            if (fd < 0)
                fd = received;
            else
                close(received);
        }
    }
    return fd >= 0 ? fd : -2;
}

} // namespace

ShellControlChannel::ShellControlChannel(QObject *parent)
    : QObject(parent)
{
}

ShellControlChannel::~ShellControlChannel()
{
    closeControl();
    delete m_listenNotifier;
    if (m_listenFd >= 0)
        close(m_listenFd);
    if (!m_path.isEmpty())
        QFile::remove(m_path);
}

bool ShellControlChannel::listen(const QString &path)
{
    const QByteArray native = path.toUtf8();
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (static_cast<size_t>(native.size()) >= sizeof(address.sun_path)) {
        qCWarning(cfWayland) << "Control socket path is too long:" << path;
        return false;
    }
    std::memcpy(address.sun_path, native.constData(), native.size());

    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m_listenFd < 0) {
        qCWarning(cfWayland) << "Cannot create the shell control socket";
        return false;
    }

    // A socket left behind by a compositor that did not shut down cleanly would
    // make bind fail; nothing else may own this path.
    QFile::remove(path);
    if (bind(m_listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || ::listen(m_listenFd, 4) != 0) {
        qCWarning(cfWayland) << "Cannot listen on the shell control socket" << path;
        close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    m_path = path;
    m_listenNotifier = new QSocketNotifier(m_listenFd, QSocketNotifier::Read, this);
    connect(m_listenNotifier, &QSocketNotifier::activated, this,
            &ShellControlChannel::onAcceptReady);
    qCInfo(cfWayland) << "Shell control channel listening on" << path;
    return true;
}

void ShellControlChannel::onAcceptReady()
{
    const int fd = accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
        return;

    closeControl();
    m_controlFd = fd;
    m_controlNotifier = new QSocketNotifier(m_controlFd, QSocketNotifier::Read, this);
    connect(m_controlNotifier, &QSocketNotifier::activated, this,
            &ShellControlChannel::onControlReady);
}

void ShellControlChannel::onControlReady()
{
    const int fd = receiveFd(m_controlFd);
    if (fd == -1) {
        // The session went away. The compositor and its clients carry on.
        closeControl();
        return;
    }
    if (fd == -2)
        return;

    Q_EMIT connectionReceived(fd);
}

void ShellControlChannel::closeControl()
{
    if (m_controlNotifier) {
        m_controlNotifier->setEnabled(false);
        delete m_controlNotifier;
        m_controlNotifier = nullptr;
    }
    if (m_controlFd >= 0) {
        close(m_controlFd);
        m_controlFd = -1;
    }
}

} // namespace cutefish::wayland
