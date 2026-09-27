// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/power_button.h"
#include "session/session_controller.h"
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <linux/input-event-codes.h>

namespace cutefish::session {
namespace {
const QString service = QStringLiteral("org.freedesktop.login1");
const QString path = QStringLiteral("/org/freedesktop/login1");
const QString manager = QStringLiteral("org.freedesktop.login1.Manager");

// Ash's PowerButtonController delays: the press that woke the system, and taps
// faster than they can be acted on.
constexpr qint64 kIgnoreAfterResumeMs = 2000;
constexpr qint64 kIgnoreRepeatedReleaseMs = 500;
} // namespace

PowerButton::PowerButton(SessionController *session, input::EventSink *next, QDBusConnection bus)
    : m_session(session)
    , m_next(next)
    , m_bus(std::move(bus))
{
}

PowerButton::~PowerButton()
{
    if (m_started)
        m_bus.disconnect(service, path, manager, QStringLiteral("PrepareForSleep"), this,
                         SLOT(prepareForSleep(bool)));
}

bool PowerButton::start()
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

void PowerButton::inhibit()
{
    auto message =
            QDBusMessage::createMethodCall(service, path, manager, QStringLiteral("Inhibit"));
    message << QStringLiteral("handle-power-key") << QStringLiteral("Cutefish")
            << QStringLiteral("The power button locks the screen") << QStringLiteral("block");
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *call) {
                const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *call;
                call->deleteLater();
                if (reply.isError()) {
                    qWarning() << "Cannot take the power button over from logind:"
                               << reply.error().message();
                    return;
                }
                m_inhibitor = reply.value();
            });
}

void PowerButton::prepareForSleep(bool sleeping)
{
    if (!sleeping)
        m_resumed.restart();
}

bool PowerButton::onKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed)
{
    if (evdevCode != KEY_POWER || !m_inhibitor.isValid())
        return m_next && m_next->onKey(timestamp, evdevCode, pressed);

    if (pressed) {
        m_ignoreRelease = m_resumed.isValid() && m_resumed.elapsed() <= kIgnoreAfterResumeMs;
        return true;
    }

    const bool repeated = m_released.isValid() && m_released.elapsed() <= kIgnoreRepeatedReleaseMs;
    m_released.restart();
    if (m_ignoreRelease || repeated)
        return true;

    m_session->requestLock();
    return true;
}

bool PowerButton::onPointerMotion(input::Timestamp timestamp, const QPointF &location)
{
    return m_next && m_next->onPointerMotion(timestamp, location);
}

bool PowerButton::onPointerButton(input::Timestamp timestamp, const QPointF &location,
                                  uint32_t evdevButton, bool pressed)
{
    return m_next && m_next->onPointerButton(timestamp, location, evdevButton, pressed);
}

bool PowerButton::onPointerAxis(input::Timestamp timestamp, const QPointF &location,
                                const QPointF &offset, bool discrete)
{
    return m_next && m_next->onPointerAxis(timestamp, location, offset, discrete);
}

bool PowerButton::onPointerAxisStop(input::Timestamp timestamp, const QPointF &location)
{
    return m_next && m_next->onPointerAxisStop(timestamp, location);
}

bool PowerButton::onTouchDown(input::Timestamp timestamp, int32_t id, const QPointF &location)
{
    return m_next && m_next->onTouchDown(timestamp, id, location);
}

bool PowerButton::onTouchMotion(input::Timestamp timestamp, int32_t id, const QPointF &location)
{
    return m_next && m_next->onTouchMotion(timestamp, id, location);
}

bool PowerButton::onTouchUp(input::Timestamp timestamp, int32_t id)
{
    return m_next && m_next->onTouchUp(timestamp, id);
}

void PowerButton::onTouchCancel(input::Timestamp timestamp)
{
    if (m_next)
        m_next->onTouchCancel(timestamp);
}
} // namespace cutefish::session
