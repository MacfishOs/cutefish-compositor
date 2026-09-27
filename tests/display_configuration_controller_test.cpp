// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/resolution_notification_controller_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: tests cover full-layout preview confirmation, timeout,
// owner loss, and confirmed-only JSON persistence.

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "display/display_configuration_controller.h"
#include "fake_native_display_delegate.h"

using namespace cutefish;

namespace {

display::DisplayId id(const char* value)
{
    return display::DisplayId(QString::fromLatin1(value));
}

display::DisplayMode mode(QSize size)
{
    return {size, 60000, 148500, size.width() + 100, size.height() + 50, 0,
            QStringLiteral("mode"), true};
}

display::DisplaySnapshot output(const char* name, QSize size)
{
    display::DisplaySnapshot value;
    value.sessionId = id(name);
    value.persistentKey = QStringLiteral("edid-") + QString::fromLatin1(name);
    value.portKey = QStringLiteral("port-") + QString::fromLatin1(name);
    value.connectorName = QString::fromLatin1(name);
    value.devicePath = QStringLiteral("/dev/dri/card0");
    value.physicalSizeMm = {600, 340};
    value.modes = {mode(size)};
    value.currentMode = value.modes.first();
    value.capabilities = display::DisplayCapability::Scale |
        display::DisplayCapability::Rotation | display::DisplayCapability::Power;
    return value;
}

display::HardwareSnapshot hardware()
{
    return {QStringLiteral("instance"), QStringLiteral("seat0"), 3,
            {output("internal", {1920, 1080}), output("external", {2560, 1440})}};
}

display::AppliedConfiguration applied(const display::HardwareSnapshot& state)
{
    return {state.compositorInstanceId,
            state.topologyGeneration,
            7,
            state.displays,
            {{id("internal"), {id("internal")}, QStringLiteral("eDP-1"),
              {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, {},
              display::DisplayRotation::Normal, true},
             {id("external"), {id("external")}, QStringLiteral("DP-1"),
              {1920, 0, 2560, 1440}, {1920, 0, 2560, 1440}, {},
              display::DisplayRotation::Normal, false}},
            {{id("external"), id("internal"), display::LayoutEdge::Right, 0}}};
}

display::DisplayConfiguration request(const display::HardwareSnapshot& state)
{
    display::DisplayConfiguration value;
    value.base = {state.compositorInstanceId, state.topologyGeneration, 7};
    value.displays = {{id("internal"), true, state.displays[0].modes[0], {},
                       display::DisplayRotation::Normal},
                      {id("external"), true, state.displays[1].modes[0],
                       *compositor::OutputScale::fromNumerator(150),
                       display::DisplayRotation::Normal}};
    value.primary = id("external");
    value.placements = {{id("internal"), id("external"), display::LayoutEdge::Left, 0}};
    return value;
}

struct Fixture {
    QTemporaryDir directory;
    display::HardwareSnapshot snapshot = hardware();
    display::DisplayManager manager;
    test::FakeNativeDisplayDelegate delegate;
    std::unique_ptr<display::DisplayConfigurationController> controller;

    Fixture()
    {
        EXPECT_TRUE(directory.isValid());
        delegate.setSnapshot(snapshot);
        EXPECT_TRUE(manager.installHardware(snapshot));
        EXPECT_TRUE(manager.installApplied(applied(snapshot)));
        controller = std::make_unique<display::DisplayConfigurationController>(
            &manager, &delegate,
            display::DisplayPreferences(directory.filePath(QStringLiteral("display.json"))));
    }
};

class DeferredDelegate : public test::FakeNativeDisplayDelegate
{
public:
    QList<ConfigureCallback> pending;
    int calls = 0;
    void configure(const display::DisplayConfiguration&, ConfigureMode,
                   ConfigureCallback callback) override
    {
        ++calls;
        pending.append(std::move(callback));
    }
    void complete()
    {
        auto callback = pending.takeFirst();
        callback({true, false, {}, std::nullopt});
    }
};

} // namespace

TEST(DisplayConfigurationController, AppliesOnlyAfterTestAndPersistsOnlyOnConfirm)
{
    Fixture fixture;
    const QString operation = fixture.controller->apply(request(fixture.snapshot),
                                                        QStringLiteral(":1.20"), 1000);
    EXPECT_TRUE(operation.startsWith(QStringLiteral("apply-")));
    EXPECT_EQ(fixture.delegate.configureCount(), 2);
    EXPECT_EQ(fixture.manager.applied()->revision, 8u);
    ASSERT_FALSE(fixture.controller->previewToken().isEmpty());
    EXPECT_FALSE(QFile::exists(fixture.directory.filePath(QStringLiteral("display.json"))));

    fixture.controller->confirm(fixture.controller->previewToken(), QStringLiteral(":1.20"));
    EXPECT_TRUE(QFile::exists(fixture.directory.filePath(QStringLiteral("display.json"))));
    display::DisplayError error;
    const auto restored = display::DisplayPreferences(
                              fixture.directory.filePath(QStringLiteral("display.json")))
                              .restore(fixture.snapshot, 8, &error);
    ASSERT_TRUE(restored) << error.message.toStdString();
    EXPECT_EQ(restored->primary, id("external"));
}

TEST(DisplayConfigurationController, RejectedConcurrentRequestsDoNotReleaseTheActiveTransaction)
{
    Fixture fixture;
    DeferredDelegate delegate;
    display::DisplayConfigurationController controller(&fixture.manager, &delegate);
    controller.apply(request(fixture.snapshot), "owner", 1000);
    ASSERT_TRUE(controller.busy());
    controller.test(request(fixture.snapshot));
    EXPECT_TRUE(controller.busy());
    controller.setPower({}, display::NativeDisplayDelegate::PowerState::Off);
    EXPECT_TRUE(controller.busy());
    controller.confirm("foreign", "other");
    EXPECT_TRUE(controller.busy());
    delegate.complete();
    ASSERT_EQ(delegate.pending.size(), 1);
    delegate.complete();
    EXPECT_FALSE(controller.busy());
    EXPECT_EQ(fixture.manager.applied()->revision, 8u);
}

TEST(DisplayConfigurationController, SnapshotInvalidationDuringTestPreventsCommit)
{
    Fixture fixture;
    DeferredDelegate delegate;
    display::DisplayConfigurationController controller(&fixture.manager, &delegate);
    display::DisplayErrorCode error = display::DisplayErrorCode::None;
    QObject::connect(&controller, &display::DisplayConfigurationController::operationFinished,
        [&](const QString&, const display::ConfigurationResult& result) { error = result.error.code; });
    controller.apply(request(fixture.snapshot), "owner", 1000);
    delegate.invalidate(fixture.snapshot.topologyGeneration + 1);
    delegate.complete();
    EXPECT_FALSE(controller.busy());
    EXPECT_EQ(error, display::DisplayErrorCode::StaleConfiguration);
    EXPECT_EQ(delegate.calls, 1);
    EXPECT_EQ(fixture.manager.applied()->revision, 7u);
}

TEST(DisplayConfigurationController, TimeoutAndOwnerLossRestorePreviousConfiguration)
{
    Fixture fixture;
    fixture.controller->apply(request(fixture.snapshot), QStringLiteral(":1.20"), 10);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.controller->previewToken().isEmpty(), 1000);
    EXPECT_EQ(fixture.manager.applied()->revision, 9u);
    EXPECT_EQ(fixture.manager.primaryDisplay()->id, id("internal"));
    EXPECT_EQ(fixture.delegate.configureCount(), 4);

    auto next = request(fixture.snapshot);
    next.base.appliedRevision = 9;
    fixture.controller->apply(next, QStringLiteral(":1.21"), 1000);
    ASSERT_FALSE(fixture.controller->previewToken().isEmpty());
    fixture.controller->ownerDisconnected(QStringLiteral(":1.21"));
    EXPECT_TRUE(fixture.controller->previewToken().isEmpty());
    EXPECT_EQ(fixture.manager.primaryDisplay()->id, id("internal"));
}

TEST(DisplayConfigurationController, RejectsStaleAndForeignPreviewTokens)
{
    Fixture fixture;
    int failures = 0;
    QObject::connect(fixture.controller.get(),
                     &display::DisplayConfigurationController::operationFinished,
                     [&failures](const QString&, const display::ConfigurationResult& result) {
                         if (!result.succeeded)
                             ++failures;
                     });
    auto stale = request(fixture.snapshot);
    stale.base.appliedRevision = 6;
    fixture.controller->apply(stale, QStringLiteral(":1.20"), 1000);
    EXPECT_EQ(fixture.delegate.configureCount(), 0);
    EXPECT_EQ(failures, 1);

    fixture.controller->apply(request(fixture.snapshot), QStringLiteral(":1.20"), 1000);
    fixture.controller->confirm(fixture.controller->previewToken(), QStringLiteral(":1.99"));
    EXPECT_FALSE(fixture.controller->previewToken().isEmpty());
    EXPECT_EQ(failures, 2);
}

TEST(DisplayConfigurationController, ImmediateChangePersistsWithoutPreviewOrOwnerLifetime)
{
    Fixture fixture;
    fixture.controller->apply(request(fixture.snapshot), QStringLiteral(":1.20"), 0);
    EXPECT_TRUE(fixture.controller->previewToken().isEmpty());
    EXPECT_EQ(fixture.manager.applied()->revision, 8u);
    EXPECT_TRUE(QFile::exists(fixture.directory.filePath(QStringLiteral("display.json"))));
    fixture.controller->ownerDisconnected(QStringLiteral(":1.20"));
    EXPECT_EQ(fixture.manager.applied()->revision, 8u);
}

TEST(DisplayConfigurationController, ModeChangesPublishSelectedModeAndRevertToPreviousSelection)
{
    Fixture fixture;
    auto hardware = fixture.snapshot;
    hardware.topologyGeneration++;
    hardware.displays[1].modes.append(mode({1920, 1080}));
    fixture.snapshot = hardware;
    fixture.delegate.setSnapshot(hardware);
    ASSERT_TRUE(fixture.manager.installHardware(hardware));
    ASSERT_TRUE(fixture.manager.installApplied(applied(hardware)));
    auto first = request(hardware);
    first.displays[1].mode = hardware.displays[1].modes[1];
    fixture.controller->apply(first, QStringLiteral(":1.20"), 0);
    ASSERT_EQ(fixture.manager.applied()->revision, 8u);
    EXPECT_EQ(fixture.manager.applied()->physicalDisplays[1].currentMode->pixelSize, QSize(1920, 1080));
    auto second = request(hardware);
    second.base.appliedRevision = 8;
    fixture.controller->apply(second, QStringLiteral(":1.20"), 1000);
    EXPECT_EQ(fixture.manager.applied()->physicalDisplays[1].currentMode->pixelSize, QSize(2560, 1440));
    fixture.controller->revert(fixture.controller->previewToken(), QStringLiteral(":1.20"));
    EXPECT_EQ(fixture.manager.applied()->physicalDisplays[1].currentMode->pixelSize, QSize(1920, 1080));
    ASSERT_TRUE(fixture.delegate.lastConfiguration());
    EXPECT_EQ(fixture.delegate.lastConfiguration()->displays[1].mode.pixelSize, QSize(1920, 1080));
}

TEST(DisplayConfigurationController, ImmediatePersistenceFailureRestoresPreviousConfiguration)
{
    Fixture fixture;
    fixture.controller = std::make_unique<display::DisplayConfigurationController>(
        &fixture.manager, &fixture.delegate, display::DisplayPreferences(fixture.directory.path()));
    bool persistenceFailed = false;
    QObject::connect(fixture.controller.get(), &display::DisplayConfigurationController::operationFinished,
        [&](const QString&, const display::ConfigurationResult& result) {
            persistenceFailed |= !result.succeeded && result.error.code == display::DisplayErrorCode::PersistenceFailed;
        });
    fixture.controller->apply(request(fixture.snapshot), QStringLiteral(":1.20"), 0);
    EXPECT_TRUE(persistenceFailed);
    EXPECT_TRUE(fixture.controller->previewToken().isEmpty());
    EXPECT_EQ(fixture.manager.primaryDisplay()->id, id("internal"));
    EXPECT_EQ(fixture.delegate.configureCount(), 4);
}

TEST(DisplayConfigurationController, PersistenceFailureCompletesOnlyAfterAsynchronousRollback)
{
    Fixture fixture;
    DeferredDelegate delegate;
    display::DisplayConfigurationController controller(
        &fixture.manager, &delegate, display::DisplayPreferences(fixture.directory.path()));
    QString completedToken;
    display::ConfigurationResult completion;
    QObject::connect(&controller, &display::DisplayConfigurationController::operationFinished,
        [&](const QString& token, const display::ConfigurationResult& result) {
            completedToken = token;
            completion = result;
        });
    const QString token = controller.apply(request(fixture.snapshot), QStringLiteral(":1.20"), 0);
    delegate.complete();
    delegate.complete();
    EXPECT_TRUE(controller.busy());
    EXPECT_TRUE(completedToken.isEmpty());
    delegate.complete();
    EXPECT_TRUE(controller.busy());
    delegate.complete();
    EXPECT_FALSE(controller.busy());
    EXPECT_EQ(completedToken, token);
    EXPECT_FALSE(completion.succeeded);
    EXPECT_EQ(completion.error.code, display::DisplayErrorCode::PersistenceFailed);
    EXPECT_EQ(fixture.manager.primaryDisplay()->id, id("internal"));
}

TEST(DisplayConfigurationController, MirrorRollbackKeepsTheSourceAsPrimary)
{
    auto snapshot = hardware();
    for (auto& physical : snapshot.displays)
        physical.capabilities |= display::DisplayCapability::SoftwareMirror;
    auto mirrored = applied(snapshot);
    mirrored.logicalDisplays.removeLast();
    mirrored.logicalDisplays[0].destinations.append(id("external"));
    mirrored.placements.clear();
    display::DisplayManager manager;
    ASSERT_TRUE(manager.installHardware(snapshot));
    ASSERT_TRUE(manager.installApplied(mirrored));
    test::FakeNativeDisplayDelegate delegate;
    display::DisplayConfigurationController controller(&manager, &delegate);
    controller.apply(request(snapshot), QStringLiteral(":1.20"), 1000);
    ASSERT_FALSE(controller.previewToken().isEmpty());
    controller.revert(controller.previewToken(), QStringLiteral(":1.20"));
    ASSERT_TRUE(delegate.lastConfiguration());
    EXPECT_EQ(delegate.lastConfiguration()->primary, id("internal"));
    EXPECT_EQ(manager.applied()->logicalDisplays.size(), 1);
}
