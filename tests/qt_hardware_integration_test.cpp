// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/qt_hardware_integration.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <QBuffer>

namespace cutefish::wayland {
namespace {

QString parse(const QByteArray& maps)
{
    QBuffer buffer;
    buffer.setData(maps);
    buffer.open(QIODevice::ReadOnly);
    return findMappedQtWaylandClient(buffer);
}

TEST(QtHardwareIntegrationTest, FindsQtWaylandClient)
{
    const QString path = parse(
            "7f10a000-7f10b000 r--p 00000000 fe:03 1311 "
            "/usr/lib/x86_64-linux-gnu/libQt6Core.so.6.8.2\n"
            "7f30a000-7f30b000 r--p 00000000 00:00 0 [heap]\n"
            "7f40a000-7f40b000 r-xp 00001000 fe:03 1313 "
            "/usr/lib/x86_64-linux-gnu/libQt6WaylandClient.so.6.8.2\n"
            "7f50a000-7f50b000 r-xp 00001000 fe:03 1314 "
            "/usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqwayland-generic.so\n");

    EXPECT_EQ(path, QStringLiteral("/usr/lib/x86_64-linux-gnu/libQt6WaylandClient.so.6.8.2"));
}

TEST(QtHardwareIntegrationTest, IgnoresQt5)
{
    const QString path = parse(
            "7f40a000-7f40b000 r-xp 00001000 fe:03 1313 "
            "/usr/lib/x86_64-linux-gnu/libQt5WaylandClient.so.5.15.15\n");

    EXPECT_TRUE(path.isEmpty());
}

// A process started before a Qt upgrade still maps the replaced file; the suffix
// keeps its path from resolving to the host's current library.
TEST(QtHardwareIntegrationTest, DeletedLibraryKeepsItsSuffix)
{
    const QString path = parse(
            "7f40a000-7f40b000 r-xp 00001000 fe:03 1313 "
            "/usr/lib/x86_64-linux-gnu/libQt6WaylandClient.so.6.8.2 (deleted)\n");

    EXPECT_TRUE(path.endsWith(QStringLiteral(" (deleted)")));
}

// This test process links no QtWaylandClient, like any non-Qt client.
TEST(QtHardwareIntegrationTest, ClientWithoutQtWaylandClientIsRefused)
{
    wl_display* display = wl_display_create();
    ASSERT_TRUE(display);
    int fds[2] = {-1, -1};
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
    wl_client* client = wl_client_create(display, fds[0]);
    ASSERT_TRUE(client);

    EXPECT_FALSE(canLoadCutefishEgl(client, false));

    wl_client_destroy(client);
    close(fds[1]);
    wl_display_destroy(display);
}

} // namespace
} // namespace cutefish::wayland
