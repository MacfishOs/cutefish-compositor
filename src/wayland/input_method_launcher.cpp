// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/input_method_launcher.h"

#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <linux/close_range.h>
#include <sys/socket.h>
#include <unistd.h>

#include <QProcess>

#include "core/logging.h"

namespace cutefish::wayland {
namespace {
constexpr int kMaxFailures = 5;
constexpr int kStableUptimeMs = 60000;
constexpr int kTerminateTimeoutMs = 3000;
} // namespace

InputMethodLauncher::InputMethodLauncher(wl_display *display, QObject *parent)
    : QObject(parent)
    , m_display(display)
{
    m_restartTimer.setSingleShot(true);
    connect(&m_restartTimer, &QTimer::timeout, this, &InputMethodLauncher::start);
}

InputMethodLauncher::~InputMethodLauncher()
{
    stop();
    for (auto *process : findChildren<QProcess *>()) {
        process->disconnect(this);
        if (!process->waitForFinished(500))
            process->kill();
        process->waitForFinished(500);
    }
}

void InputMethodLauncher::setCommand(const QStringList &command)
{
    if (command == m_command && (m_client || m_restartTimer.isActive()))
        return;
    stop();
    m_command = command;
    m_failures = 0;
    start();
}

void InputMethodLauncher::restart()
{
    if (m_command.isEmpty())
        return;
    stop();
    m_failures = 0;
    start();
}

void InputMethodLauncher::start()
{
    if (m_command.isEmpty() || m_client)
        return;

    int fds[2] {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds)) {
        qCWarning(cfWayland) << "Cannot create the input method connection";
        return;
    }
    m_client = wl_client_create(m_display, fds[0]);
    if (!m_client) {
        close(fds[0]);
        close(fds[1]);
        qCWarning(cfWayland) << "Cannot adopt the input method connection";
        return;
    }
    m_destroy.owner = this;
    m_destroy.listener.notify = [](wl_listener *listener, void *) {
        reinterpret_cast<DestroyListener *>(listener)->owner->connectionLost();
    };
    wl_client_add_destroy_listener(m_client, &m_destroy.listener);

    auto *process = new QProcess(this);
    const int child = fds[1];
    process->setChildProcessModifier([child] {
        if (close_range(3, ~0u, CLOSE_RANGE_CLOEXEC) < 0)
            _exit(127);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        for (int signal : {SIGINT, SIGTERM, SIGPIPE, SIGHUP, SIGCHLD})
            ::signal(signal, SIG_DFL);
        fcntl(child, F_SETFD, 0);
    });
    auto environment = m_environment;
    environment.insert(QStringLiteral("WAYLAND_SOCKET"), QString::number(child));
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) {
        qCInfo(cfWayland) << "Input method process" << process->program() << "exited with" << code;
        if (m_process == process)
            m_process = nullptr;
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        qCWarning(cfWayland) << "Cannot start input method" << process->program();
        if (m_process == process)
            m_process = nullptr;
        process->deleteLater();
    });
    m_process = process;
    m_uptime.start();
    process->start(m_command.first(), m_command.mid(1));
    // The child holds the only other end, so its exit closes the connection.
    close(child);
    qCInfo(cfWayland) << "Started input method" << m_command;
    emit runningChanged();
}

void InputMethodLauncher::stop()
{
    m_restartTimer.stop();
    if (auto *client = m_client) {
        wl_list_remove(&m_destroy.listener.link);
        m_client = nullptr;
        wl_client_destroy(client);
        emit runningChanged();
    }
    if (auto *process = std::exchange(m_process, nullptr))
        terminate(process);
}

void InputMethodLauncher::terminate(QProcess *process)
{
    if (process->state() == QProcess::NotRunning)
        return;
    process->terminate();
    QTimer::singleShot(kTerminateTimeoutMs, process, [process] {
        process->kill();
    });
}

void InputMethodLauncher::connectionLost()
{
    wl_list_remove(&m_destroy.listener.link);
    m_client = nullptr;
    emit runningChanged();

    // A process without its connection cannot serve input; do not leave a duplicate running.
    if (auto *process = std::exchange(m_process, nullptr))
        terminate(process);
    if (m_uptime.isValid() && m_uptime.elapsed() >= kStableUptimeMs)
        m_failures = 0;
    if (++m_failures > kMaxFailures) {
        qCWarning(cfWayland) << "Input method" << m_command
                             << "keeps losing its connection; not restarting it";
        return;
    }
    const int delay = std::min(m_restartDelay << (m_failures - 1), m_restartDelay * 16);
    qCWarning(cfWayland) << "Input method connection lost; restarting in" << delay << "ms";
    m_restartTimer.start(delay);
}

} // namespace cutefish::wayland
