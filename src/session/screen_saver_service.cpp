// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/screen_saver_service.h"
#include "session/session_controller.h"
#include <QDBusError>
#include <QDebug>

namespace cutefish::session {
namespace {
const QString serviceName = QStringLiteral("org.freedesktop.ScreenSaver");
const QString objectPaths[] = {QStringLiteral("/ScreenSaver"),
                               QStringLiteral("/org/freedesktop/ScreenSaver")};
} // namespace

ScreenSaverService::ScreenSaverService(SessionController *session, QDBusConnection bus)
    : m_session(session)
    , m_bus(std::move(bus))
    , m_watcher(QString(), m_bus, QDBusServiceWatcher::WatchForUnregistration)
    , m_active(session->isBlocked())
{
    m_session->addObserver(this);
    // A caller that leaves the bus takes all of its cookies with it.
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [this](const QString &owner) {
                for (auto it = m_cookies.begin(); it != m_cookies.end();) {
                    if (it.value() == owner) {
                        it = m_cookies.erase(it);
                        if (m_release)
                            m_release();
                    } else {
                        ++it;
                    }
                }
                m_watcher.removeWatchedService(owner);
            });
}

ScreenSaverService::~ScreenSaverService()
{
    m_session->removeObserver(this);
    for (const auto &request : m_pending)
        m_bus.send(request.createErrorReply(QDBusError::Failed,
                                            QStringLiteral("Compositor is shutting down")));
    if (m_registered) {
        m_bus.unregisterService(serviceName);
        for (const QString &path : objectPaths)
            m_bus.unregisterObject(path);
    }
}

bool ScreenSaverService::registerService()
{
    if (m_registered)
        return true;
    for (const QString &path : objectPaths) {
        if (m_bus.registerObject(path, this,
                                 QDBusConnection::ExportScriptableSlots
                                         | QDBusConnection::ExportScriptableSignals))
            continue;
        qWarning() << "Cannot export ScreenSaver at" << path << ":" << m_bus.lastError().message();
        // Only the paths exported here: another object may hold this one.
        for (const QString &exported : objectPaths) {
            if (exported == path)
                break;
            m_bus.unregisterObject(exported);
        }
        return false;
    }
    if (!m_bus.registerService(serviceName)) {
        for (const QString &path : objectPaths)
            m_bus.unregisterObject(path);
        qWarning() << "Cannot own ScreenSaver service:" << m_bus.lastError().message();
        return false;
    }
    m_registered = true;
    return true;
}

void ScreenSaverService::setInhibition(std::function<void()> acquire,
                                       std::function<void()> release)
{
    m_acquire = std::move(acquire);
    m_release = std::move(release);
}

void ScreenSaverService::Lock()
{
    if (m_session->state() == SessionState::Locked)
        return;
    if (m_session->state() == SessionState::Login
        || m_session->state() == SessionState::Unlocking) {
        if (calledFromDBus())
            sendErrorReply(QDBusError::Failed,
                           QStringLiteral("Session is not available for locking"));
        return;
    }
    // Callers may suspend or power off only after every output is securely covered.
    if (calledFromDBus()) {
        setDelayedReply(true);
        m_pending.push_back(message());
    }
    m_session->requestLock();
    if (m_session->secureContainer() && m_session->secureContainer()->outputs().isEmpty())
        m_session->notifyFramePresented();
}

bool ScreenSaverService::GetActive() const
{
    return m_session->isBlocked();
}

uint ScreenSaverService::Inhibit(const QString &, const QString &)
{
    if (!calledFromDBus())
        return 0;
    if (!m_acquire || !m_release) {
        sendErrorReply(QDBusError::NotSupported,
                       QStringLiteral("Idle inhibition is not available in this session"));
        return 0;
    }
    const QString owner = message().service();
    do {
        ++m_nextCookie;
    } while (!m_nextCookie || m_cookies.contains(m_nextCookie));
    m_watcher.addWatchedService(owner);
    m_cookies.insert(m_nextCookie, owner);
    m_acquire();
    return m_nextCookie;
}

void ScreenSaverService::UnInhibit(uint cookie)
{
    if (!calledFromDBus())
        return;
    auto it = m_cookies.find(cookie);
    if (it == m_cookies.end() || it.value() != message().service())
        return;
    const QString owner = it.value();
    m_cookies.erase(it);
    if (!m_cookies.values().contains(owner))
        m_watcher.removeWatchedService(owner);
    if (m_release)
        m_release();
}

void ScreenSaverService::onSessionStateChanged(SessionState state)
{
    const bool active = isSessionBlocked(state);
    if (active != m_active) {
        m_active = active;
        emit ActiveChanged(active);
    }
    if (state == SessionState::Locked) {
        const auto pending = std::move(m_pending);
        m_pending.clear();
        for (const auto &request : pending)
            m_bus.send(request.createReply());
    }
}
} // namespace cutefish::session
