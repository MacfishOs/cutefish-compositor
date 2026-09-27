// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/manager/test/fake_display_snapshot_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: tests stable string identities and exact timing keys.

#include <gtest/gtest.h>

#include "display/display_types.h"

using namespace cutefish::display;

namespace {

DisplayMode mode(QSize size = {1920, 1080}, int refresh = 60000)
{
    return {size, refresh, 148500, 2200, 1125, 0, QStringLiteral("preferred"), true};
}

DisplaySnapshot snapshot(QString id, QString port)
{
    DisplaySnapshot value;
    value.sessionId = DisplayId(std::move(id));
    value.persistentKey = QStringLiteral("edid:vendor:model:serial");
    value.portKey = std::move(port);
    value.connectorName = QStringLiteral("DP-1");
    value.devicePath = QStringLiteral("/dev/dri/card0");
    value.physicalSizeMm = QSize(600, 340);
    value.modes = {mode()};
    value.currentMode = value.modes.first();
    value.nativeMode = value.modes.first();
    return value;
}

} // namespace

TEST(DisplayTypes, ModeIdentityRetainsTiming)
{
    DisplayMode first = mode();
    DisplayMode second = first;
    second.pixelClockKHz += 1;
    EXPECT_TRUE(first.isValid());
    EXPECT_NE(first.stableKey(), second.stableKey());

    second.horizontalTotal = 100;
    EXPECT_FALSE(second.isValid());
}

TEST(DisplayTypes, SnapshotRequiresUniqueSessionAndPortIdentity)
{
    HardwareSnapshot hardware {
        QStringLiteral("instance"),
        QStringLiteral("seat0"),
        4,
        {snapshot(QStringLiteral("session-a"), QStringLiteral("card0-DP-1")),
         snapshot(QStringLiteral("session-b"), QStringLiteral("card0-DP-2"))}};
    EXPECT_TRUE(hardware.isValid());

    hardware.displays[1].sessionId = hardware.displays[0].sessionId;
    EXPECT_FALSE(hardware.isValid());
    hardware.displays[1].sessionId = DisplayId(QStringLiteral("session-b"));
    hardware.displays[1].portKey = hardware.displays[0].portKey;
    EXPECT_FALSE(hardware.isValid());
}

TEST(DisplayTypes, CurrentModeMustComeFromModeList)
{
    DisplaySnapshot display = snapshot(QStringLiteral("session-a"), QStringLiteral("card0-DP-1"));
    display.currentMode->refreshMilliHz = 59940;
    EXPECT_FALSE(display.isValid());
}
