// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/drm_gpu_display_manager_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: tests cover partial cross-device rollback, transfer
// fallback, complete tile groups, and hardware clone capability gating.

#include <gtest/gtest.h>

#include "backend/drm/drm_multi_device_planner.h"

using namespace cutefish;

TEST(DrmMultiDevicePlanner, RollsBackEarlierDeviceAfterPartialCommitFailure)
{
    int firstCommits = 0;
    int firstRollbacks = 0;
    uint64_t generation = 4;
    const auto result = backend::DrmMultiDeviceCommitCoordinator::run({{QStringLiteral("card0"),
                                                                        [] {
                                                                            return true;
                                                                        },
                                                                        [&] {
                                                                            ++firstCommits;
                                                                            return true;
                                                                        },
                                                                        [&] {
                                                                            ++firstRollbacks;
                                                                            return true;
                                                                        }},
                                                                       {QStringLiteral("card1"),
                                                                        [] {
                                                                            return true;
                                                                        },
                                                                        [] {
                                                                            return false;
                                                                        },
                                                                        [] {
                                                                            return true;
                                                                        }}},
                                                                      4, [&] {
                                                                          return generation;
                                                                      });
    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.rolledBack);
    EXPECT_EQ(result.failedDevice, QStringLiteral("card1"));
    EXPECT_EQ(firstCommits, 1);
    EXPECT_EQ(firstRollbacks, 1);
}

TEST(DrmMultiDevicePlanner, CancelsWhenADeviceDisappearsBetweenTestAndCommit)
{
    uint64_t generation = 8;
    const auto result = backend::DrmMultiDeviceCommitCoordinator::run({{QStringLiteral("card0"),
                                                                        [&] {
                                                                            ++generation;
                                                                            return true;
                                                                        },
                                                                        [] {
                                                                            return true;
                                                                        },
                                                                        [] {
                                                                            return true;
                                                                        }}},
                                                                      8, [&] {
                                                                          return generation;
                                                                      });
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.failedDevice, QStringLiteral("card0"));
}

TEST(DrmMultiDevicePlanner, ChoosesBoundedCrossGpuFallbacks)
{
    EXPECT_EQ(backend::chooseCrossGpuTransfer({false, true, true, true, 8000000}),
              backend::CrossGpuTransfer::DmaBufImport);
    EXPECT_EQ(backend::chooseCrossGpuTransfer({false, false, false, true, 1280 * 720}),
              backend::CrossGpuTransfer::CpuCopy);
    // A 4K screen still gets a copy; anything larger does not.
    EXPECT_EQ(backend::chooseCrossGpuTransfer({false, false, false, true, 3840 * 2160}),
              backend::CrossGpuTransfer::CpuCopy);
    EXPECT_EQ(backend::chooseCrossGpuTransfer({false, false, false, true, 5120 * 2880}),
              backend::CrossGpuTransfer::Unavailable);
}

TEST(DrmMultiDevicePlanner, GroupsOnlyCompleteTiledMonitors)
{
    const auto grouped =
            backend::groupDrmTiles({{10, QStringLiteral("wall"), 0, 0, 2, 1, {1920, 2160}},
                                    {11, QStringLiteral("wall"), 1, 0, 2, 1, {1920, 2160}}});
    ASSERT_TRUE(grouped);
    ASSERT_EQ(grouped->size(), 1);
    EXPECT_EQ(grouped->first().combinedSize, QSize(3840, 2160));
    EXPECT_EQ(grouped->first().connectors, QList<uint32_t>({10, 11}));
    EXPECT_FALSE(backend::groupDrmTiles({{10, QStringLiteral("wall"), 0, 0, 2, 1, {1920, 2160}}}));
    EXPECT_TRUE(backend::hardwareCloneEligible(QStringLiteral("card0"), QStringLiteral("card0"),
                                               QStringLiteral("1080p60"), QStringLiteral("1080p60"),
                                               0b11, 0b10));
}

TEST(DrmMultiDevicePlanner, DeviceTestFailurePreventsEveryCommit)
{
    int commits = 0;
    const auto commit = [&] {
        ++commits;
        return true;
    };
    const auto result = backend::DrmMultiDeviceCommitCoordinator::run({{"card0",
                                                                        [] {
                                                                            return true;
                                                                        },
                                                                        commit,
                                                                        {}},
                                                                       {"card1",
                                                                        [] {
                                                                            return false;
                                                                        },
                                                                        commit,
                                                                        {}}},
                                                                      1, [] {
                                                                          return 1;
                                                                      });
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.rolledBack);
    EXPECT_EQ(result.failedDevice, "card1");
    EXPECT_EQ(commits, 0);
}

TEST(DrmMultiDevicePlanner, RollbackFailureStillAttemptsAllEarlierDevicesInReverseOrder)
{
    QStringList rollbacks;
    const auto yes = [] {
        return true;
    };
    const auto result =
            backend::DrmMultiDeviceCommitCoordinator::run({{"card0", yes, yes,
                                                            [&] {
                                                                rollbacks.append("card0");
                                                                return true;
                                                            }},
                                                           {"card1", yes, yes,
                                                            [&] {
                                                                rollbacks.append("card1");
                                                                return false;
                                                            }},
                                                           {"card2",
                                                            yes,
                                                            [] {
                                                                return false;
                                                            },
                                                            {}}},
                                                          1, [] {
                                                              return 1;
                                                          });
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.rolledBack);
    EXPECT_EQ(rollbacks, QStringList({"card1", "card0"}));
    EXPECT_TRUE(result.message.contains("incomplete"));
}

TEST(DrmMultiDevicePlanner, InvalidationDuringFinalCommitCannotReportSuccess)
{
    uint64_t generation = 2;
    int rollbacks = 0;
    const auto result = backend::DrmMultiDeviceCommitCoordinator::run({{"card0",
                                                                        [] {
                                                                            return true;
                                                                        },
                                                                        [&] {
                                                                            ++generation;
                                                                            return true;
                                                                        },
                                                                        [&] {
                                                                            ++rollbacks;
                                                                            return false;
                                                                        }}},
                                                                      2, [&] {
                                                                          return generation;
                                                                      });
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.rolledBack);
    EXPECT_EQ(rollbacks, 1);
    EXPECT_EQ(result.failedDevice, "card0");
}
