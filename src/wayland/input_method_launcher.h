// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <wayland-server-core.h>

#include <QElapsedTimer>
#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>

#include "core/process_environment.h"

class QProcess;

namespace cutefish::wayland {

// Runs the selected input method on a Wayland connection the compositor
// creates, like the Xwayland child; that connection is the only one allowed to
// act as the input method. A lost connection is restarted with backoff.
class InputMethodLauncher : public QObject
{
    Q_OBJECT

public:
    explicit InputMethodLauncher(wl_display *display, QObject *parent = nullptr);
    ~InputMethodLauncher() override;
    InputMethodLauncher(const InputMethodLauncher &) = delete;
    InputMethodLauncher &operator=(const InputMethodLauncher &) = delete;

    // An empty command stops the input method. Setting the current command
    // again restarts a method that gave up.
    void setCommand(const QStringList &command);

    void setEnvironment(const QProcessEnvironment &environment) { m_environment = environment; }
    // Starts the current command again, with the current environment.
    void restart();

    void setRestartDelay(int milliseconds) { m_restartDelay = milliseconds; }

    bool isInputMethod(const wl_client *client) const { return client && client == m_client; }

    bool isRunning() const { return m_client; }

signals:
    void runningChanged();

private:
    struct DestroyListener {
        wl_listener listener;
        InputMethodLauncher *owner;
    };

    void start();
    void stop();
    void connectionLost();
    void terminate(QProcess *process);

    wl_display *m_display;
    wl_client *m_client = nullptr;
    DestroyListener m_destroy {};
    QStringList m_command;
    QProcessEnvironment m_environment = childEnvironment();
    QProcess *m_process = nullptr;
    QTimer m_restartTimer;
    QElapsedTimer m_uptime;
    int m_restartDelay = 1000;
    int m_failures = 0;
};

} // namespace cutefish::wayland
