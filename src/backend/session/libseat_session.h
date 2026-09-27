// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_BACKEND_SESSION_LIBSEAT_SESSION_H_
#define CUTEFISH_BACKEND_SESSION_LIBSEAT_SESSION_H_

#include <memory>

#include "backend/session/session.h"

struct libseat;
class QSocketNotifier;

namespace cutefish::backend {

// Session backed by libseat (seatd or logind).
class LibseatSession : public Session
{
    Q_OBJECT

public:
    ~LibseatSession() override;

    // Opens the seat and pumps libseat until the initial enable event arrives.
    // Returns nullptr if no seat could be acquired.
    static std::unique_ptr<LibseatSession> create(QObject* parent = nullptr);

    QString seatName() const override { return m_seatName; }
    bool isActive() const override { return m_active; }

    int openDevice(const QString& path, int* deviceId) override;
    void closeDevice(int deviceId) override;

private:
    explicit LibseatSession(QObject* parent);

    void dispatch();

    static void onEnableSeat(libseat* seat, void* userData);
    static void onDisableSeat(libseat* seat, void* userData);

    libseat* m_seat = nullptr;
    QSocketNotifier* m_notifier = nullptr;
    QString m_seatName;
    bool m_active = false;
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_SESSION_LIBSEAT_SESSION_H_
