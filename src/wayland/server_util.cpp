// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/wayland/server_util.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "server_util.h"

#include <QHash>

#include <cmath>

#include <wayland-server-protocol.h>

#include "server.h"

namespace cutefish::wayland {

namespace {

QHash<compositor::Surface*, wl_resource*>& surfaceResources()
{
    static QHash<compositor::Surface*, wl_resource*> map;
    return map;
}

} // namespace

compositor::Surface* surfaceFromResource(wl_resource* resource)
{
    return resource ? getUserDataAs<compositor::Surface>(resource) : nullptr;
}

wl_resource* getSurfaceResource(compositor::Surface* surface)
{
    return surfaceResources().value(surface, nullptr);
}

void setSurfaceResource(compositor::Surface* surface, wl_resource* resource)
{
    if (resource)
        surfaceResources().insert(surface, resource);
    else
        surfaceResources().remove(surface);
}

void setImplementation(wl_resource* resource, const void* implementation)
{
    wl_resource_set_implementation(resource, implementation, nullptr, nullptr);
}

uint32_t timePointToMilliseconds(std::chrono::steady_clock::time_point tp)
{
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(tp.time_since_epoch()).count());
}

uint32_t nowInMilliseconds()
{
    return timePointToMilliseconds(std::chrono::steady_clock::now());
}

double clientScale(wl_client* client)
{
    Server* server = client ? Server::serverForDisplay(wl_client_get_display(client)) : nullptr;
    return server ? server->clientScale(client) : 1;
}

QRect clientToLogical(const QRect& rect, double scale)
{
    if (scale == 1 || rect.isEmpty())
        return rect;
    const int left = int(std::floor(rect.x() / scale));
    const int top = int(std::floor(rect.y() / scale));
    const int right = int(std::ceil((rect.x() + rect.width()) / scale));
    const int bottom = int(std::ceil((rect.y() + rect.height()) / scale));
    return QRect(QPoint(left, top), QPoint(right - 1, bottom - 1));
}

QPoint clientToLogical(const QPoint& point, double scale)
{
    return scale == 1 ? point : QPoint(qRound(point.x() / scale), qRound(point.y() / scale));
}

QPointF logicalToClient(const QPointF& point, double scale)
{
    return point * scale;
}

bool isClientDestroyed(wl_client* client)
{
    // A client always has a display, and the display always outlives it.
    wl_display* display = wl_client_get_display(client);
    Server* server = Server::serverForDisplay(display);
    return server ? server->isClientDestroyed(client) : true;
}

} // namespace cutefish::wayland
