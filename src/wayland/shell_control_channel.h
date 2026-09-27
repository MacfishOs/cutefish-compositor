// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WAYLAND_SHELL_CONTROL_CHANNEL_H_
#define CUTEFISH_WAYLAND_SHELL_CONTROL_CHANNEL_H_

#include <QObject>
#include <QString>

class QSocketNotifier;

namespace cutefish::wayland {

// The compositor end of a persistent control channel the session holds open.
//
// The session, not the compositor, launches its privileged clients: for every
// launch and relaunch it creates a Wayland socketpair, passes one end here over
// SCM_RIGHTS, and gives the other to the new process as WAYLAND_SOCKET. The
// compositor turns the fd it receives into a trusted wl_client. Nothing about
// the client's identity is taken on trust from the client itself.
//
// The channel is a plain SOCK_STREAM UNIX socket. It carries file descriptors
// and nothing else; no window-management traffic ever crosses it.
class ShellControlChannel : public QObject
{
    Q_OBJECT

public:
    explicit ShellControlChannel(QObject *parent = nullptr);
    ~ShellControlChannel() override;

    // Binds and listens on `path`, removing a stale socket left by a previous
    // compositor. Returns false and logs on failure; the compositor runs on
    // without a shell in that case.
    bool listen(const QString &path);

    QString path() const { return m_path; }

Q_SIGNALS:
    // A new Wayland connection fd for the trusted client this channel serves.
    // The receiver takes ownership of `fd`.
    void connectionReceived(int fd);

private:
    void onAcceptReady();
    void onControlReady();
    void closeControl();

    int m_listenFd = -1;
    // One session at a time. A second connection replaces the first, which is
    // what a session restart looks like from here.
    int m_controlFd = -1;
    QSocketNotifier *m_listenNotifier = nullptr;
    QSocketNotifier *m_controlNotifier = nullptr;
    QString m_path;
};

} // namespace cutefish::wayland

#endif // CUTEFISH_WAYLAND_SHELL_CONTROL_CHANNEL_H_
