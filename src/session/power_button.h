// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "input/event_sink.h"
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QObject>

namespace cutefish::session {
class SessionController;

// A tap on the power button locks the session; the lock screen then goes dark on
// its own idle timeout. Holding it is left to the firmware's forced power-off.
// logind is asked to leave the key alone, or it would power the machine off.
// Upstream: ash/system/power/power_button_controller.cc, the tablet-mode tap.
class PowerButton : public QObject, public input::EventSink
{
    Q_OBJECT
public:
    // `next` receives every other event.
    PowerButton(SessionController *session, input::EventSink *next,
                QDBusConnection bus = QDBusConnection::systemBus());
    ~PowerButton() override;

    // Takes the key over from logind and follows its PrepareForSleep. Until the
    // inhibitor is held the key passes on untouched.
    bool start();

    bool holdsInhibitor() const { return m_inhibitor.isValid(); }

    // input::EventSink:
    bool onPointerMotion(input::Timestamp timestamp, const QPointF &location) override;
    bool onPointerButton(input::Timestamp timestamp, const QPointF &location,
                         uint32_t evdevButton, bool pressed) override;
    bool onPointerAxis(input::Timestamp timestamp, const QPointF &location,
                       const QPointF &offset, bool discrete) override;
    bool onPointerAxisStop(input::Timestamp timestamp, const QPointF &location) override;
    bool onKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed) override;
    bool onTouchDown(input::Timestamp timestamp, int32_t id, const QPointF &location) override;
    bool onTouchMotion(input::Timestamp timestamp, int32_t id, const QPointF &location) override;
    bool onTouchUp(input::Timestamp timestamp, int32_t id) override;
    void onTouchCancel(input::Timestamp timestamp) override;

private slots:
    void prepareForSleep(bool sleeping);

private:
    void inhibit();

    SessionController *m_session;
    input::EventSink *m_next;
    QDBusConnection m_bus;
    QDBusUnixFileDescriptor m_inhibitor;
    QElapsedTimer m_resumed;
    QElapsedTimer m_released;
    bool m_ignoreRelease = false;
    bool m_started = false;
};
} // namespace cutefish::session
