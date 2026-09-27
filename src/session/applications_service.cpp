// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/applications_service.h"

#include <QDBusError>
#include <QDebug>
#include <utility>

#include "wm/quit_all.h"

namespace cutefish::session {

namespace {
const QString serviceName = QStringLiteral("com.cutefish.Applications1");
const QString objectPath = QStringLiteral("/com/cutefish/Applications1");

QString outcomeName(wm::QuitAll::Outcome outcome)
{
    switch (outcome) {
    case wm::QuitAll::Outcome::Quit:
        return QStringLiteral("quit");
    case wm::QuitAll::Outcome::Cancelled:
        return QStringLiteral("cancelled");
    case wm::QuitAll::Outcome::Unresponsive:
        return QStringLiteral("unresponsive");
    }
    return QString();
}
} // namespace

ApplicationsService::ApplicationsService(wm::QuitAll *quitAll,
                                         std::function<void(bool)> setEnding, QDBusConnection bus)
    : m_quitAll(quitAll)
    , m_setEnding(std::move(setEnding))
    , m_bus(std::move(bus))
{
}

ApplicationsService::~ApplicationsService()
{
    for (const auto &request : m_pending)
        m_bus.send(request.createErrorReply(QDBusError::Failed,
                                            QStringLiteral("Compositor is shutting down")));
    if (m_registered) {
        m_bus.unregisterService(serviceName);
        m_bus.unregisterObject(objectPath);
    }
}

bool ApplicationsService::registerService()
{
    if (m_registered)
        return true;
    if (!m_bus.registerObject(objectPath, this, QDBusConnection::ExportScriptableSlots)) {
        qWarning() << "Cannot export the applications service:" << m_bus.lastError().message();
        return false;
    }
    if (!m_bus.registerService(serviceName)) {
        qWarning() << "Cannot own the applications service:" << m_bus.lastError().message();
        m_bus.unregisterObject(objectPath);
        return false;
    }
    m_registered = true;
    return true;
}

QString ApplicationsService::QuitAll(QString &)
{
    if (!calledFromDBus())
        return QString();
    setDelayedReply(true);
    m_pending.push_back(message());
    if (m_quitAll->isRunning())
        return QString();

    if (m_setEnding)
        m_setEnding(true);
    m_quitAll->start([this](const wm::QuitAll::Result &result) {
        if (result.outcome != wm::QuitAll::Outcome::Quit && m_setEnding)
            m_setEnding(false);
        const auto pending = std::exchange(m_pending, {});
        for (const auto &request : pending)
            m_bus.send(request.createReply({outcomeName(result.outcome), result.applicationId}));
    });
    return QString();
}

} // namespace cutefish::session
