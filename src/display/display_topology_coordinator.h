// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "display/callback_native_display_delegate.h"
#include "display/display_configuration_controller.h"
#include <QTimer>
#include <functional>

namespace cutefish::display {

class DisplayTopologyCoordinator : public QObject
{
    Q_OBJECT
public:
    using Probe = std::function<std::optional<HardwareSnapshot>()>;
    DisplayTopologyCoordinator(DisplayManager *manager, CallbackNativeDisplayDelegate *delegate,
                               DisplayConfigurationController *controller, Probe probe,
                               DisplayPreferences preferences = DisplayPreferences(),
                               QObject *parent = nullptr);
    void invalidate();
    // The first reconciliation. Unlike invalidate(), it does not carry over
    // the startup layout, which nobody chose: unsaved displays get defaults.
    void start();

signals:
    void reconciled(bool succeeded);

private:
    void schedule();
    void run();
    DisplayConfiguration candidate(const HardwareSnapshot &hardware, uint64_t revision) const;
    DisplayManager *m_manager;
    CallbackNativeDisplayDelegate *m_delegate;
    DisplayConfigurationController *m_controller;
    Probe m_probe;
    DisplayPreferences m_preferences;
    QTimer m_retry;
    std::optional<AppliedConfiguration> m_previous;
    bool m_pending = false;
    bool m_running = false;
    QString m_operationToken;
    int m_attempts = 0;
    uint64_t m_revision = 0;
};

} // namespace cutefish::display
