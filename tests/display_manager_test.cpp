// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/manager/display_manager_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: tests cover immutable applied batches and global Qt
// coordinates without Ash display objects.

#include <gtest/gtest.h>

#include <QSignalSpy>

#include "display/display_manager.h"

using namespace cutefish;

namespace {

display::DisplayId id(const char *value)
{
    return display::DisplayId(QString::fromLatin1(value));
}

display::DisplayMode mode(QSize size)
{
    return {size, 60000, 148500, size.width() + 100, size.height() + 50, 0, QStringLiteral("mode"),
            true};
}

display::DisplaySnapshot physical(const char *name, QSize size)
{
    display::DisplaySnapshot result;
    result.sessionId = id(name);
    result.persistentKey = QStringLiteral("edid-") + QString::fromLatin1(name);
    result.portKey = QStringLiteral("port-") + QString::fromLatin1(name);
    result.connectorName = QString::fromLatin1(name);
    result.devicePath = QStringLiteral("/dev/dri/card0");
    result.physicalSizeMm = {600, 340};
    result.modes = {mode(size)};
    result.currentMode = result.modes.first();
    return result;
}

display::LogicalDisplay logical(const char *name, QRect bounds, bool primary)
{
    return {id(name),
            {id(name)},
            QStringLiteral("Cutefish-") + QString::fromLatin1(name),
            bounds,
            bounds.adjusted(0, 24, 0, 0),
            {},
            display::DisplayRotation::Normal,
            primary};
}

} // namespace

TEST(DisplayManager, PublishesCoherentBatchesAndRejectsStaleState)
{
    display::DisplayManager manager;
    QSignalSpy started(&manager, &display::DisplayManager::displayChangeStarted);
    QSignalSpy changed(&manager, &display::DisplayManager::displaysChanged);
    ASSERT_TRUE(manager.installHardware({QStringLiteral("instance"),
                                         QStringLiteral("seat0"),
                                         1,
                                         {physical("a", {100, 100}), physical("b", {120, 80})}}));
    ASSERT_TRUE(manager.installApplied(
            {QStringLiteral("instance"),
             1,
             1,
             {},
             {logical("a", {0, 0, 100, 100}, true), logical("b", {100, 20, 120, 80}, false)},
             {{id("b"), id("a"), display::LayoutEdge::Right, 20}}}));
    EXPECT_EQ(started.count(), 2);
    EXPECT_EQ(changed.count(), 2);
    EXPECT_EQ(manager.desktopBounds(), QRect(0, 0, 220, 100));

    display::DisplayError error;
    EXPECT_FALSE(manager.installApplied({QStringLiteral("instance"), 1, 1, {}, {}, {}}, &error));
    EXPECT_EQ(error.code, display::DisplayErrorCode::StaleConfiguration);
}

TEST(DisplayManager, SelectsDisplaysAcrossEdgesGapsAndSpanningWindows)
{
    display::DisplayManager manager;
    ASSERT_TRUE(manager.installHardware({QStringLiteral("instance"),
                                         QStringLiteral("seat0"),
                                         1,
                                         {physical("a", {100, 100}), physical("b", {100, 100})}}));
    ASSERT_TRUE(manager.installApplied(
            {QStringLiteral("instance"),
             1,
             1,
             {},
             {logical("a", {-100, 0, 100, 100}, false), logical("b", {20, 0, 100, 100}, true)},
             {}}));
    EXPECT_EQ(manager.displayAt(QPointF(-0.001, 20))->id, id("a"));
    EXPECT_EQ(manager.displayAt(QPointF(20, 20))->id, id("b"));
    EXPECT_EQ(manager.displayAt(QPointF(10, 20)), nullptr);
    EXPECT_EQ(manager.nearestDisplay(QPointF(18, 20))->id, id("b"));
    EXPECT_EQ(manager.displayForRect(QRect(-30, 10, 80, 40))->id, id("b"));
    EXPECT_EQ(manager.primaryDisplay()->id, id("b"));
}

TEST(DisplayManager, BuildsTheSharedTransformFromLogicalAndPhysicalState)
{
    display::DisplayManager manager;
    auto output = physical("a", {1920, 1080});
    ASSERT_TRUE(manager.installHardware(
            {QStringLiteral("instance"), QStringLiteral("seat0"), 1, {output}}));
    auto logicalOutput = logical("a", {-720, 40, 720, 1280}, true);
    logicalOutput.scale = *compositor::OutputScale::fromNumerator(180);
    logicalOutput.rotation = display::DisplayRotation::Rotate90;
    logicalOutput.workArea = logicalOutput.bounds;
    ASSERT_TRUE(manager.installApplied(
            {QStringLiteral("instance"), 1, 1, {output}, {logicalOutput}, {}}));
    const auto transform = manager.transformFor(id("a"));
    ASSERT_TRUE(transform);
    EXPECT_EQ(transform->logicalBounds(), logicalOutput.bounds);
    const QPointF point(-500, 400);
    const QPointF roundTrip = transform->nativeToGlobal(transform->globalToNative(point));
    EXPECT_NEAR(roundTrip.x(), point.x(), 0.001);
    EXPECT_NEAR(roundTrip.y(), point.y(), 0.001);
}

// ash's TouchDeviceManager ties an internal touchscreen to the internal panel,
// wherever that panel sits on the desktop and whichever display is primary.
TEST(DisplayManager, TheTouchscreenFollowsTheInternalPanel)
{
    display::DisplayManager manager;
    auto external = physical("a", {1920, 1080});
    auto panel = physical("e", {2160, 1440});
    panel.connectionType = display::DisplayConnectionType::Internal;
    ASSERT_TRUE(manager.installHardware(
            {QStringLiteral("instance"), QStringLiteral("seat0"), 1, {external, panel}}));
    auto primary = logical("a", {0, 0, 1920, 1080}, true);
    auto internal = logical("e", {1920, 0, 1440, 960}, false);
    internal.scale = *compositor::OutputScale::fromNumerator(180);
    ASSERT_TRUE(manager.installApplied({QStringLiteral("instance"), 1, 1, {external, panel},
                                        {primary, internal}, {}}));

    const auto corner = manager.touchscreenToGlobal({0, 0});
    ASSERT_TRUE(corner);
    EXPECT_NEAR(corner->x(), 1920, 0.01);
    EXPECT_NEAR(corner->y(), 0, 0.01);
    const auto middle = manager.touchscreenToGlobal({0.5, 0.5});
    ASSERT_TRUE(middle);
    EXPECT_NEAR(middle->x(), 1920 + 720, 0.01);
    EXPECT_NEAR(middle->y(), 480, 0.01);
    // The far edge is still on the panel, not past it.
    EXPECT_TRUE(manager.touchscreenToGlobal({1, 1}));

    // Turned, the panel still reports in its own orientation: the point maps
    // back onto the same native pixel.
    internal.rotation = display::DisplayRotation::Rotate90;
    internal.bounds = {1920, 0, 960, 1440};
    internal.workArea = internal.bounds;
    ASSERT_TRUE(manager.installApplied({QStringLiteral("instance"), 1, 2, {external, panel},
                                        {primary, internal}, {}}));
    const auto transform = manager.mappingFor(id("e"))->transform();
    for (const QPointF normalized : {QPointF(0.1, 0.2), QPointF(0.8, 0.3)}) {
        const auto global = manager.touchscreenToGlobal(normalized);
        ASSERT_TRUE(global);
        EXPECT_TRUE(internal.bounds.contains(global->toPoint()));
        const QPointF native = transform.logicalToNative(*global);
        EXPECT_NEAR(native.x(), normalized.x() * 2160, 0.5);
        EXPECT_NEAR(native.y(), normalized.y() * 1440, 0.5);
    }

    // Without an internal panel the primary display takes it.
    panel.connectionType = display::DisplayConnectionType::External;
    internal.rotation = display::DisplayRotation::Normal;
    internal.bounds = {1920, 0, 1440, 960};
    internal.workArea = internal.bounds;
    ASSERT_TRUE(manager.installApplied({QStringLiteral("instance"), 1, 3, {external, panel},
                                        {primary, internal}, {}}));
    const auto onPrimary = manager.touchscreenToGlobal({0.5, 0.5});
    ASSERT_TRUE(onPrimary);
    EXPECT_NEAR(onPrimary->x(), 960, 0.01);
    EXPECT_NEAR(onPrimary->y(), 540, 0.01);
}

// A 3:2 panel driven at 16:9 shows the picture between black bars; the glass
// still spans the whole panel, so a touch lands where the picture is drawn.
TEST(DisplayManager, TheTouchscreenFollowsThePanelFitter)
{
    display::DisplayManager manager;
    auto panel = physical("e", {2160, 1440});
    panel.connectionType = display::DisplayConnectionType::Internal;
    panel.modes.append(mode({1920, 1080}));
    panel.modes.append(mode({1600, 1200}));
    panel.nativeMode = panel.modes[0];
    panel.currentMode = panel.modes[1];
    ASSERT_TRUE(manager.installHardware(
            {QStringLiteral("instance"), QStringLiteral("seat0"), 1, {panel}}));
    auto internal = logical("e", {0, 0, 1920, 1080}, true);
    ASSERT_TRUE(manager.installApplied(
            {QStringLiteral("instance"), 1, 1, {panel}, {internal}, {}}));

    // The picture is 2160x1215, 112.5 pixels down the glass.
    const auto middle = manager.touchscreenToGlobal({0.5, 0.5});
    ASSERT_TRUE(middle);
    EXPECT_NEAR(middle->x(), 960, 0.01);
    EXPECT_NEAR(middle->y(), 540, 0.01);
    const auto nearBottom = manager.touchscreenToGlobal({0.25, (112.5 + 1215 * 0.9) / 1440});
    ASSERT_TRUE(nearBottom);
    EXPECT_NEAR(nearBottom->x(), 480, 0.01);
    EXPECT_NEAR(nearBottom->y(), 972, 0.01);
    // A touch on a bar reaches the nearest edge of the picture.
    const auto onBar = manager.touchscreenToGlobal({0.5, 0.02});
    ASSERT_TRUE(onBar);
    EXPECT_NEAR(onBar->y(), 0, 0.01);

    // 4:3 is pillarboxed: the picture is 1920x1440, 120 pixels in.
    panel.currentMode = panel.modes[2];
    internal.bounds = {0, 0, 1600, 1200};
    internal.workArea = internal.bounds;
    ASSERT_TRUE(manager.installApplied(
            {QStringLiteral("instance"), 1, 2, {panel}, {internal}, {}}));
    const auto left = manager.touchscreenToGlobal({(120 + 1920 * 0.1) / 2160, 0.5});
    ASSERT_TRUE(left);
    EXPECT_NEAR(left->x(), 160, 0.01);
    EXPECT_NEAR(left->y(), 600, 0.01);
}
