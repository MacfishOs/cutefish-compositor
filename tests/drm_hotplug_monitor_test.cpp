// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/host/drm_display_host_manager_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: tests the bounded Qt hotplug debounce.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>

#include "backend/drm/drm_hotplug_monitor.h"

using namespace cutefish;

TEST(DrmHotplugMonitor, CoalescesEventBurst)
{
    backend::DrmHotplugMonitor monitor;
    QSignalSpy requested(&monitor, &backend::DrmHotplugMonitor::reprobeRequested);
    monitor.notifyForTest();
    monitor.notifyForTest();
    monitor.notifyForTest();
    ASSERT_TRUE(requested.wait(500));
    EXPECT_EQ(requested.count(), 1);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
