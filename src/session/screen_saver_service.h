// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "session/session_state.h"
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QObject>
#include <functional>
#include <vector>

namespace cutefish::session {
class SessionController;

// The one org.freedesktop.ScreenSaver object, at /ScreenSaver and
// /org/freedesktop/ScreenSaver: a second object at either path fails to register.
class ScreenSaverService : public QObject, protected QDBusContext, public SessionObserver
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    explicit ScreenSaverService(SessionController *session,
                                QDBusConnection bus = QDBusConnection::sessionBus());
    ~ScreenSaverService() override;
    bool registerService();
    void onSessionStateChanged(SessionState state) override;

    // Inhibit counts against the compositor's idle inhibition through these.
    // Without them, as in the greeter, Inhibit is refused.
    void setInhibition(std::function<void()> acquire, std::function<void()> release);

public slots:
    Q_SCRIPTABLE void Lock();
    Q_SCRIPTABLE bool GetActive() const;
    Q_SCRIPTABLE uint Inhibit(const QString &application_name, const QString &reason_for_inhibit);
    Q_SCRIPTABLE void UnInhibit(uint cookie);

signals:
    Q_SCRIPTABLE void ActiveChanged(bool active);

private:
    SessionController *m_session;
    QDBusConnection m_bus;
    QDBusServiceWatcher m_watcher;
    std::vector<QDBusMessage> m_pending;
    std::function<void()> m_acquire;
    std::function<void()> m_release;
    QHash<uint, QString> m_cookies;
    uint m_nextCookie = 0;
    bool m_registered = false;
    bool m_active;
};
} // namespace cutefish::session
