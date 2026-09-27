// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/sleep_lock.h"
#include "session/session_controller.h"
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QTimer>

namespace cutefish::session {
namespace {
const QString service = QStringLiteral("org.freedesktop.login1");
const QString path = QStringLiteral("/org/freedesktop/login1");
const QString manager = QStringLiteral("org.freedesktop.login1.Manager");
} // namespace

SleepLock::SleepLock(SessionController *session, QDBusConnection bus)
    : m_session(session)
    , m_bus(std::move(bus))
{
    m_session->addObserver(this);
}

SleepLock::~SleepLock()
{
    m_session->removeObserver(this);
    if (m_started)
        m_bus.disconnect(service, path, manager, QStringLiteral("PrepareForSleep"), this,
                         SLOT(prepareForSleep(bool)));
}

bool SleepLock::start()
{
    if (m_started)
        return true;
    if (!m_bus.connect(service, path, manager, QStringLiteral("PrepareForSleep"), this,
                       SLOT(prepareForSleep(bool)))) {
        qWarning() << "Cannot follow logind's PrepareForSleep:" << m_bus.lastError().message();
        return false;
    }
    m_started = true;
    inhibit();
    return true;
}

void SleepLock::inhibit()
{
    if (m_inhibiting || m_inhibitor.isValid())
        return;
    m_inhibiting = true;
    auto message = QDBusMessage::createMethodCall(service, path, manager, QStringLiteral("Inhibit"));
    message << QStringLiteral("sleep") << QStringLiteral("Cutefish")
            << QStringLiteral("Lock the screen before sleeping") << QStringLiteral("delay");
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *call) {
                m_inhibiting = false;
                const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *call;
                // The reply holds a copy of the descriptor too.
                call->deleteLater();
                if (reply.isError()) {
                    qWarning() << "Cannot delay sleep to lock the screen:"
                               << reply.error().message();
                    return;
                }
                // Too late for this sleep; the next wake asks again.
                if (!m_sleeping)
                    m_inhibitor = reply.value();
            });
}

void SleepLock::prepareForSleep(bool sleeping)
{
    m_sleeping = sleeping;
    if (!sleeping) {
        inhibit();
        return;
    }
    // An unlock in progress finishes first; onSessionStateChanged locks after it.
    if (!m_session->isBlocked())
        m_session->requestLock();
    releaseWhenLocked();
}

void SleepLock::onSessionStateChanged(SessionState state)
{
    if (!m_sleeping)
        return;
    if (state == SessionState::Active) {
        // Not from inside the controller's own state change.
        QTimer::singleShot(0, this, [this] {
            if (m_sleeping && !m_session->isBlocked())
                m_session->requestLock();
        });
        return;
    }
    releaseWhenLocked();
}

void SleepLock::releaseWhenLocked()
{
    const SessionState state = m_session->state();
    if (state == SessionState::Locked || state == SessionState::Login)
        m_inhibitor = QDBusUnixFileDescriptor();
}
} // namespace cutefish::session
