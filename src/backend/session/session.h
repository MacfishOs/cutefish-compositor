// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_BACKEND_SESSION_SESSION_H_
#define CUTEFISH_BACKEND_SESSION_SESSION_H_

#include <QObject>
#include <QString>

namespace cutefish::backend {

// Owns privileged device access. Devices are opened through the session rather
// than directly, so the compositor never needs to run as root and the seat
// manager can revoke access across VT switches.
class Session : public QObject
{
    Q_OBJECT

public:
    explicit Session(QObject* parent = nullptr) : QObject(parent) {}
    ~Session() override = default;

    virtual QString seatName() const = 0;

    // False while another session owns the seat. No DRM ioctl that requires
    // master may be issued while inactive.
    virtual bool isActive() const = 0;

    // Returns a file descriptor owned by the caller-visible session, or -1.
    // `deviceId` receives the handle needed by closeDevice().
    virtual int openDevice(const QString& path, int* deviceId) = 0;
    virtual void closeDevice(int deviceId) = 0;

Q_SIGNALS:
    // Emitted before the seat is actually released, so listeners can stop
    // touching devices first.
    void activeChanged(bool active);
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_SESSION_SESSION_H_
