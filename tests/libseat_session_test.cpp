// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>

#include <fcntl.h>

#include <cerrno>
#include <memory>

#include "backend/session/libseat_session.h"

using namespace cutefish;

namespace {

bool isOpen(int fd)
{
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

// libseat's noop backend opens devices itself, so no seat manager is needed;
// like logind and seatd it leaves the descriptor it hands out to the caller.
std::unique_ptr<backend::LibseatSession> createSession()
{
    qputenv("LIBSEAT_BACKEND", "noop");
    return backend::LibseatSession::create();
}

} // namespace

TEST(LibseatSession, ClosingADeviceClosesItsDescriptor)
{
    auto session = createSession();
    ASSERT_TRUE(session);
    int deviceId = -1;
    const int fd = session->openDevice(QStringLiteral("/dev/null"), &deviceId);
    ASSERT_GE(fd, 0);
    ASSERT_GE(deviceId, 0);
    EXPECT_TRUE(isOpen(fd));

    session->closeDevice(deviceId);
    EXPECT_FALSE(isOpen(fd));
}

TEST(LibseatSession, ClosingOneDeviceLeavesTheOthersOpen)
{
    auto session = createSession();
    ASSERT_TRUE(session);
    int firstId = -1;
    int secondId = -1;
    const int first = session->openDevice(QStringLiteral("/dev/null"), &firstId);
    const int second = session->openDevice(QStringLiteral("/dev/zero"), &secondId);
    ASSERT_GE(first, 0);
    ASSERT_GE(second, 0);

    session->closeDevice(firstId);
    EXPECT_FALSE(isOpen(first));
    EXPECT_TRUE(isOpen(second));

    session->closeDevice(secondId);
    EXPECT_FALSE(isOpen(second));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
