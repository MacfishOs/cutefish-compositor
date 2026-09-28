// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "backend/session/libseat_session.h"

#include <QSocketNotifier>

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

// libseat.h ships without extern "C" guards.
extern "C" {
#include <libseat.h>
}

#include "core/logging.h"

namespace cutefish::backend {

namespace {

void logHandler(libseat_log_level level, const char* format, va_list args)
{
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    if (level == LIBSEAT_LOG_LEVEL_ERROR)
        qCWarning(cfSession, "libseat: %s", buffer);
    else
        qCDebug(cfSession, "libseat: %s", buffer);
}

// The signals main.cpp blocks for its signalfd, pending before it reads them.
bool stopPending()
{
    sigset_t pending;
    if (sigpending(&pending) != 0)
        return false;
    return sigismember(&pending, SIGTERM) == 1 || sigismember(&pending, SIGINT) == 1
            || sigismember(&pending, SIGHUP) == 1;
}

} // namespace

LibseatSession::LibseatSession(QObject* parent)
    : Session(parent)
{
}

LibseatSession::~LibseatSession()
{
    delete m_notifier;
    m_notifier = nullptr;
    if (m_seat)
        libseat_close_seat(m_seat);
}

std::unique_ptr<LibseatSession> LibseatSession::create(QObject* parent)
{
    libseat_set_log_handler(logHandler);
    libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);

    std::unique_ptr<LibseatSession> session(new LibseatSession(parent));

    // libseat keeps this pointer and calls through it for the seat's lifetime,
    // so it cannot live on this stack frame.
    static const libseat_seat_listener listener = {
        .enable_seat = &LibseatSession::onEnableSeat,
        .disable_seat = &LibseatSession::onDisableSeat,
    };

    session->m_seat = libseat_open_seat(&listener, session.get());
    if (!session->m_seat) {
        qCCritical(cfSession) << "Failed to open a seat; is seatd running or is "
                                 "this a logind session?";
        return nullptr;
    }

    // The seat is unusable until the first enable event, which never comes to a
    // session nobody activates; a stop must still end the wait.
    while (!session->m_active) {
        if (stopPending()) {
            qCInfo(cfSession) << "Stopped while waiting for the seat";
            return nullptr;
        }
        if (libseat_dispatch(session->m_seat, 100) < 0) {
            qCCritical(cfSession) << "Failed to dispatch libseat while waiting for the seat";
            return nullptr;
        }
    }

    const char* name = libseat_seat_name(session->m_seat);
    session->m_seatName = QString::fromUtf8(name ? name : "");

    const int fd = libseat_get_fd(session->m_seat);
    if (fd < 0) {
        qCCritical(cfSession) << "libseat provided no event fd";
        return nullptr;
    }

    session->m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, session.get());
    connect(session->m_notifier, &QSocketNotifier::activated, session.get(),
            [s = session.get()] { s->dispatch(); });

    qCInfo(cfSession) << "Seat:" << session->m_seatName;
    return session;
}

void LibseatSession::dispatch()
{
    if (libseat_dispatch(m_seat, 0) >= 0 || errno == EINTR || errno == EAGAIN)
        return;
    // The seat connection is gone and its fd stays readable: watching it
    // further would only spin. Open devices keep working; switching does not.
    qCCritical(cfSession) << "Lost the seat connection:" << strerror(errno);
    m_notifier->setEnabled(false);
}

int LibseatSession::openDevice(const QString& path, int* deviceId)
{
    int fd = -1;
    const int id = libseat_open_device(m_seat, path.toUtf8().constData(), &fd);
    if (id < 0) {
        qCWarning(cfSession) << "Failed to open device" << path << "through the seat";
        return -1;
    }
    m_deviceFds.insert(id, fd);
    if (deviceId)
        *deviceId = id;
    return fd;
}

void LibseatSession::closeDevice(int deviceId)
{
    if (deviceId < 0)
        return;
    if (libseat_close_device(m_seat, deviceId) < 0)
        qCWarning(cfSession) << "Failed to close device" << deviceId;
    // Only afterwards: with logind the id is this descriptor, which
    // libseat_close_device still stats to release the device.
    const auto entry = m_deviceFds.constFind(deviceId);
    if (entry != m_deviceFds.cend()) {
        ::close(*entry);
        m_deviceFds.erase(entry);
    }
}

void LibseatSession::onEnableSeat(libseat*, void* userData)
{
    auto* self = static_cast<LibseatSession*>(userData);
    if (self->m_active)
        return;
    self->m_active = true;
    qCInfo(cfSession) << "Seat enabled";
    Q_EMIT self->activeChanged(true);
}

void LibseatSession::onDisableSeat(libseat* seat, void* userData)
{
    auto* self = static_cast<LibseatSession*>(userData);
    if (self->m_active) {
        self->m_active = false;
        qCInfo(cfSession) << "Seat disabled";
        Q_EMIT self->activeChanged(false);
    }
    // Only acknowledge once listeners have stopped using their devices.
    libseat_disable_seat(seat);
}

} // namespace cutefish::backend
