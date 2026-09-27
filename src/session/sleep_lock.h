// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "session/session_state.h"
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QObject>

namespace cutefish::session {
class SessionController;

// Locks the session before the system sleeps, however the sleep was asked
// for: a menu, the lid, idle or systemctl. A logind delay inhibitor holds the
// sleep back until the lock is on every output, so waking never shows the
// desktop. Upstream: ash/system/power/power_event_observer.cc, which delays
// suspend the same way until the lock screen has been composited.
class SleepLock : public QObject, public SessionObserver
{
    Q_OBJECT
public:
    explicit SleepLock(SessionController *session,
                       QDBusConnection bus = QDBusConnection::systemBus());
    ~SleepLock() override;

    // Follows logind's PrepareForSleep and takes the inhibitor.
    bool start();

    bool holdsInhibitor() const { return m_inhibitor.isValid(); }

    void onSessionStateChanged(SessionState state) override;

private slots:
    void prepareForSleep(bool sleeping);

private:
    void inhibit();
    void releaseWhenLocked();

    SessionController *m_session;
    QDBusConnection m_bus;
    QDBusUnixFileDescriptor m_inhibitor;
    bool m_inhibiting = false;
    bool m_sleeping = false;
    bool m_started = false;
};
} // namespace cutefish::session
