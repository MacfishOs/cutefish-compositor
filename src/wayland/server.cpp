// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/wayland/server.cc
//           chromium/src/components/exo/wayland/wayland_watcher.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "server.h"

#include <QAbstractEventDispatcher>
#include <QSocketNotifier>
#include <QThread>

#include <cstdarg>
#include <cstdio>
#include <map>

#include "core/logging.h"

namespace cutefish::wayland {

namespace {

std::map<wl_display*, Server*>& displayServerMap()
{
    static std::map<wl_display*, Server*> map;
    return map;
}

void waylandLog(const char* fmt, va_list argp)
{
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, argp);
    qCWarning(cfWayland, "libwayland: %s", buffer);
}

} // namespace

Server::Server(QObject* parent)
    : QObject(parent)
{
    wl_log_set_handler_server(waylandLog);

    m_display.reset(wl_display_create());
    if (!m_display) {
        qCCritical(cfWayland) << "Failed to create the Wayland display";
        return;
    }

    displayServerMap().emplace(m_display.get(), this);
    m_serialTracker = std::make_unique<SerialTracker>(m_display.get());
    m_clientTracker = std::make_unique<ClientTracker>(m_display.get());
}

Server::~Server()
{
    stop();

    if (m_display) {
        displayServerMap().erase(m_display.get());
        m_serialTracker->shutdown();
    }
}

Server* Server::serverForDisplay(wl_display* display)
{
    auto& map = displayServerMap();
    auto it = map.find(display);
    return it == map.end() ? nullptr : it->second;
}

bool Server::addSocket(const QString& name)
{
    if (!m_display)
        return false;

    if (name.isEmpty()) {
        const char* socket = wl_display_add_socket_auto(m_display.get());
        if (!socket) {
            qCCritical(cfWayland) << "Failed to create a Wayland socket";
            return false;
        }
        m_socketName = QString::fromUtf8(socket);
    } else {
        if (wl_display_add_socket(m_display.get(), name.toUtf8().constData()) != 0) {
            qCCritical(cfWayland) << "Failed to create the Wayland socket" << name;
            return false;
        }
        m_socketName = name;
    }

    qCInfo(cfWayland) << "Listening on Wayland socket" << m_socketName;
    return true;
}

bool Server::start()
{
    if (m_notifier)
        return true;

    const int fd = fileDescriptor();
    if (fd < 0)
        return false;

    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Server::onSocketActivated);

    // Nothing wakes the Qt event loop when the server itself queues events, so
    // flush whatever is pending just before Qt blocks on the fd.
    if (QAbstractEventDispatcher* dispatcher = QThread::currentThread()->eventDispatcher())
        connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, &Server::flush);

    return true;
}

void Server::stop()
{
    if (!m_notifier)
        return;

    if (QAbstractEventDispatcher* dispatcher = QThread::currentThread()->eventDispatcher())
        disconnect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, &Server::flush);

    delete m_notifier;
    m_notifier = nullptr;
}

int Server::fileDescriptor() const
{
    if (!m_display)
        return -1;
    wl_event_loop* eventLoop = wl_display_get_event_loop(m_display.get());
    return eventLoop ? wl_event_loop_get_fd(eventLoop) : -1;
}

bool Server::dispatch(std::chrono::milliseconds timeout)
{
    if (!m_display)
        return false;

    wl_event_loop* eventLoop = wl_display_get_event_loop(m_display.get());
    if (wl_event_loop_dispatch(eventLoop, static_cast<int>(timeout.count())) < 0) {
        qCWarning(cfWayland) << "Wayland event loop dispatch failed";
        return false;
    }
    return true;
}

void Server::flush()
{
    if (!m_display)
        return;

    // wl_display_flush_clients() is avoided for the same reason as upstream
    // Exo: it can crash while a client is disconnecting.
    wl_client* client = nullptr;
    wl_list* allClients = wl_display_get_client_list(m_display.get());
    wl_client_for_each(client, allClients)
    {
        if (!isClientDestroyed(client))
            wl_client_flush(client);
    }
}

bool Server::isClientDestroyed(wl_client* client) const
{
    return m_clientTracker ? m_clientTracker->isClientDestroyed(client) : true;
}

void Server::onSocketActivated()
{
    dispatch(std::chrono::milliseconds(0));
    flush();
}

} // namespace cutefish::wayland
