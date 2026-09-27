// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "compositor/compositor.h"
#include "compositor/output.h"
#include "compositor/surface.h"
#include "cursor/cursor.h"
#include "display/callback_native_display_delegate.h"
#include "display/display_configuration_controller.h"
#include "display/display_runtime.h"
#include "display/display_topology_coordinator.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/pointer.h"
#include "input/pointer_delegate.h"
#include "input/seat.h"
#include "scene/scene.h"
#include "session/session_controller.h"
#include "shm_test_util.h"
#include "wayland/protocols/wl_output.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace cutefish;

namespace {

class PointerRecorder : public input::PointerDelegate
{
public:
    compositor::Surface *target = nullptr;
    compositor::Surface *entered = nullptr;
    QPointF local;
    QList<bool> buttons;

    void onPointerDestroying(input::Pointer *) override { }

    bool canAcceptPointerEventsForSurface(compositor::Surface *surface) const override
    {
        return surface == target;
    }

    void onPointerEnter(compositor::Surface *surface, const QPointF &point,
                        input::ButtonFlags) override
    {
        entered = surface;
        local = point;
    }

    void onPointerLeave(compositor::Surface *) override { }

    void onPointerMotion(input::Timestamp, const QPointF &point) override { local = point; }

    void onPointerButton(input::Timestamp, input::ButtonFlags, bool pressed) override
    {
        buttons.append(pressed);
    }

    void onPointerScroll(input::Timestamp, const QPointF &, bool) override { }

    void onFingerScrollStop(input::Timestamp) override { }

    void onPointerFrame() override { }
};

class DisplayRuntimeTest : public testing::Test
{
protected:
    scene::Scene scene;
    compositor::Compositor compositor {scene.root()};
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    wm::WindowManager wm {&compositor, &seat};
    cursor::Cursor cursor {seat, *scene.root()};
    display::DisplayManager manager;
    compositor::Output first;
    compositor::Output second;
    wl_display *server = wl_display_create();
    std::vector<wl_global *> globals {nullptr, nullptr};
    wl_global *primaryGlobal = nullptr;
    bool updating = false;
    bool failCommit = false;
    QTemporaryDir directory;
    std::unique_ptr<display::DisplayRuntime> runtime;
    std::unique_ptr<display::CallbackNativeDisplayDelegate> delegate;
    std::unique_ptr<display::DisplayConfigurationController> controller;
    testsupport::FakeRenderer renderer;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 20);
    std::vector<std::unique_ptr<compositor::Surface>> surfaces;

    void SetUp() override
    {
        first.name = QStringLiteral("Virtual-1");
        second.name = QStringLiteral("Virtual-2");
        first.model = first.name;
        second.model = second.name;
        first.physicalSizeMm = second.physicalSizeMm = {0, 0};
        first.pixelSize = second.pixelSize = {1920, 1080};
        first.refreshMilliHz = second.refreshMilliHz = 60000;
        second.position = {1920, 0};
        wm.setDisplayManager(&manager);
        display::DisplayRuntime::initialize(manager, nullptr, {&first, &second},
                                            QStringLiteral("runtime-test"), compositor, input);
        ASSERT_TRUE(manager.applied());
        runtime = std::make_unique<display::DisplayRuntime>(
                manager, nullptr, QList<compositor::Output *> {&first, &second}, globals,
                primaryGlobal, server, compositor, input, wm, scene, cursor, nullptr, updating);
        delegate = std::make_unique<display::CallbackNativeDisplayDelegate>(
                *manager.hardware(),
                [this](const auto &request, auto mode) {
                    if (failCommit && mode == display::NativeDisplayDelegate::ConfigureMode::Commit)
                        return display::ConfigurationResult {
                                false,
                                false,
                                {display::DisplayErrorCode::CommitFailed,
                                 QStringLiteral("Injected commit failure"),
                                 {}},
                                std::nullopt};
                    return runtime->configure(request, mode);
                },
                [this](const auto &ids, auto state) {
                    return runtime->setPower(ids, state);
                });
        makeController(directory.filePath(QStringLiteral("display.json")));
    }

    void makeController(const QString &path)
    {
        controller = std::make_unique<display::DisplayConfigurationController>(
                &manager, delegate.get(), display::DisplayPreferences(path));
    }

    bool replaceTestHardware(display::HardwareSnapshot hardware)
    {
        auto applied = *manager.applied();
        ++hardware.topologyGeneration;
        applied.topologyGeneration = hardware.topologyGeneration;
        applied.physicalDisplays = hardware.displays;
        if (!manager.installHardware(hardware) || !manager.installApplied(applied))
            return false;
        delegate->setSnapshot(hardware);
        return true;
    }

    void TearDown() override
    {
        controller.reset();
        delegate.reset();
        runtime.reset();
        seat.shutdown();
        for (auto *global : globals)
            wayland::removeOutputGlobal(global);
        wl_display_destroy(server);
    }

    display::DisplayConfiguration
    request(int modeIndex = 0, int numerator = 120,
            display::DisplayRotation rotation = display::DisplayRotation::Normal)
    {
        display::DisplayConfiguration result;
        result.base = {manager.hardware()->compositorInstanceId,
                       manager.hardware()->topologyGeneration, manager.applied()->revision};
        for (const auto &physical : manager.hardware()->displays)
            result.displays.append({physical.sessionId, true, physical.modes.at(modeIndex),
                                    *compositor::OutputScale::fromNumerator(numerator), rotation});
        result.primary = result.displays.first().display;
        result.placements = {
                {result.displays.last().display, result.primary, display::LayoutEdge::Left, -40}};
        return result;
    }

    display::ConfigurationResult apply(const display::DisplayConfiguration &request)
    {
        QSignalSpy completed(controller.get(),
                             &display::DisplayConfigurationController::operationFinished);
        const auto token = controller->apply(request, QStringLiteral("internal"), 0);
        EXPECT_TRUE(completed.isEmpty());
        if (!completed.wait(2000)) {
            ADD_FAILURE() << "No asynchronous operation completion";
            return {};
        }
        EXPECT_EQ(completed.size(), 1);
        EXPECT_EQ(completed.first().first().toString(), token);
        return qvariant_cast<display::ConfigurationResult>(completed.first().at(1));
    }

    void expectPublished()
    {
        ASSERT_TRUE(manager.applied());
        const QList<compositor::Output *> models {&first, &second};
        QList<QRect> topology;
        for (int i = 0; i < models.size(); ++i) {
            const auto id = manager.hardware()->displays.at(i).sessionId;
            const auto *logical = manager.display(id);
            ASSERT_NE(logical, nullptr);
            EXPECT_EQ(models[i]->position, logical->bounds.topLeft());
            EXPECT_EQ(models[i]->logicalSize(), logical->bounds.size());
            const auto transform = manager.transformFor(id);
            ASSERT_TRUE(transform);
            EXPECT_EQ(transform->nativeSize(), models[i]->pixelSize);
            EXPECT_EQ(transform->logicalBounds(), logical->bounds);
            topology.append(logical->bounds);
            EXPECT_NE(globals[i], nullptr);
        }
        EXPECT_EQ(input.topology(), topology);
        EXPECT_EQ(wm.outputBounds(), manager.desktopBounds());
        EXPECT_EQ(scene.size(), manager.desktopBounds().size());
        EXPECT_FALSE(updating);
    }

    wm::Window *window(QRect bounds)
    {
        auto surface = compositor.createSurface();
        auto buffer = pool->createBuffer(bounds.size(), 0, bounds.width() * 4,
                                         render::PixelFormat::Bgrx8888, nullptr);
        surface->attach(buffer, {});
        surface->damage(QRect(QPoint(), bounds.size()));
        surface->commit();
        compositor.updateTextures(renderer);
        auto *window = wm.createWindow(surface.get(), {.requestedSize = bounds.size()});
        window->setBounds(bounds);
        surfaces.push_back(std::move(surface));
        return window;
    }
};

TEST_F(DisplayRuntimeTest, ConsecutiveChangesPublishBeforeObserversAndDeliverAbsoluteClicks)
{
    int notifications = 0;
    QObject::connect(&manager, &display::DisplayManager::displaysChanged, &manager,
                     [this, &notifications] {
                         ++notifications;
                         expectPublished();
                     });
    int step = 0;
    for (auto rotation : {display::DisplayRotation::Normal, display::DisplayRotation::Rotate90,
                          display::DisplayRotation::Rotate180, display::DisplayRotation::Rotate270,
                          display::DisplayRotation::Normal}) {
        ASSERT_TRUE(apply(request(step % 3, step % 2 ? 180 : 120, rotation)).succeeded);
        expectPublished();
        const auto id = manager.hardware()->displays.last().sessionId;
        const auto *logical = manager.display(id);
        ASSERT_LT(logical->bounds.x(), 0);
        const QPointF target = logical->bounds.center();
        const QRect desktop = manager.desktopBounds();
        auto *client = window(QRect(target.toPoint() - QPoint(10, 10), QSize(20, 20)));
        PointerRecorder recorder;
        recorder.target = client->rootSurface();
        input::Pointer pointer(&recorder, &seat);
        input.onPointerMotionNormalized(input::Timestamp {},
                                        {(target.x() - desktop.x()) / desktop.width(),
                                         (target.y() - desktop.y()) / desktop.height()});
        EXPECT_EQ(input.pointerPosition(), target);
        EXPECT_EQ(recorder.entered, recorder.target);
        EXPECT_EQ(recorder.local, QPointF(10, 10));
        input.onPointerButton(input::Timestamp {}, BTN_LEFT, true);
        input.onPointerButton(input::Timestamp {}, BTN_LEFT, false);
        EXPECT_EQ(recorder.buttons, (QList<bool> {true, false}));
        const auto native = manager.transformFor(id)->globalToNative(target);
        EXPECT_EQ(manager.transformFor(id)->nativeToGlobal(native), target);
        EXPECT_EQ(manager.outputLocalToGlobal(id, *manager.globalToOutputLocal(id, target)),
                  target);
        ++step;
    }
    EXPECT_EQ(notifications, 5);
    EXPECT_EQ(manager.hardware()->displays.first().currentMode->pixelSize, QSize(1920, 1080));
}

TEST_F(DisplayRuntimeTest, PublicationReportsTheLargestScaleForXwayland)
{
    QList<double> reported;
    runtime->maximumScaleChanged = [&](double scale) {
        reported.append(scale);
    };
    auto mixed = request();
    ASSERT_GE(mixed.displays.size(), 2);
    mixed.displays.last().scale = *compositor::OutputScale::fromNumerator(150);
    ASSERT_TRUE(apply(mixed).succeeded);
    ASSERT_FALSE(reported.isEmpty());
    EXPECT_DOUBLE_EQ(reported.last(), 1.25);
    ASSERT_TRUE(apply(request()).succeeded);
    EXPECT_DOUBLE_EQ(reported.last(), 1.0);
}

TEST_F(DisplayRuntimeTest, FirstMinimizeResolvesDockAfterDisplayPublication)
{
    auto *client = window({100, 100, 300, 200});
    auto *dock = window({400, 900, 300, 80});
    const QRect icon(30, 10, 48, 48);
    client->setMinimizedGeometry(dock, icon);
    ASSERT_TRUE(apply(request(2, 180, display::DisplayRotation::Rotate90)).succeeded);
    const auto bounds = manager.applied()->logicalDisplays.last().bounds;
    dock->setBounds(QRect(bounds.bottomLeft() - QPoint(0, 79), QSize(300, 80)));
    client->minimize();
    EXPECT_EQ(client->minimizedGeometry(), icon.translated(dock->contentBounds().topLeft()));
    client->restore();
    wm.destroyWindow(dock);
    EXPECT_TRUE(client->minimizedGeometry().isEmpty());
}

TEST_F(DisplayRuntimeTest, CommitFailurePreservesPublishedRevisionAndPreferences)
{
    ASSERT_TRUE(apply(request(1)).succeeded);
    const auto before = *manager.applied();
    QFile saved(directory.filePath(QStringLiteral("display.json")));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const auto bytes = saved.readAll();
    saved.close();
    failCommit = true;
    const auto result = apply(request(2, 180));
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::CommitFailed);
    EXPECT_EQ(manager.applied()->revision, before.revision);
    expectPublished();
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_EQ(saved.readAll(), bytes);
}

TEST_F(DisplayRuntimeTest, PersistenceFailureCompletesOnlyAfterRuntimeRollback)
{
    const auto before = *manager.applied();
    QFile blocker(directory.filePath(QStringLiteral("blocker")));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    makeController(blocker.fileName() + QStringLiteral("/display.json"));
    const auto result = apply(request(2, 180));
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(manager.applied()->revision, before.revision + 2);
    EXPECT_EQ(manager.applied()->physicalDisplays.first().currentMode->pixelSize,
              before.physicalDisplays.first().currentMode->pixelSize);
    EXPECT_EQ(manager.desktopBounds(), QRect(0, 0, 3840, 1080));
    expectPublished();
}

TEST_F(DisplayRuntimeTest, TemporaryPowerPreservesDisabledOutputsAndSavedModeIntent)
{
    auto candidate = request(1);
    candidate.displays.last().enabled = false;
    candidate.placements.clear();
    ASSERT_TRUE(apply(candidate).succeeded);
    const auto power = [this](display::NativeDisplayDelegate::PowerState state) {
        const auto revision = manager.applied()->revision;
        QSignalSpy completed(controller.get(),
                             &display::DisplayConfigurationController::operationFinished);
        controller->setPower({}, state);
        EXPECT_TRUE(completed.isEmpty());
        ASSERT_TRUE(completed.wait(2000));
        ASSERT_TRUE(qvariant_cast<display::ConfigurationResult>(completed.first().at(1)).succeeded);
        EXPECT_EQ(manager.applied()->revision, revision);
        EXPECT_EQ(manager.applied()->logicalDisplays.size(), 1);
        EXPECT_EQ(globals.back(), nullptr);
    };
    power(display::NativeDisplayDelegate::PowerState::Off);
    candidate = request(2, 180);
    candidate.displays.last().enabled = false;
    candidate.placements.clear();
    ASSERT_TRUE(apply(candidate).succeeded);
    power(display::NativeDisplayDelegate::PowerState::On);
    display::DisplayPreferences preferences(directory.filePath(QStringLiteral("display.json")));
    const auto saved = preferences.restore(*manager.hardware(), manager.applied()->revision);
    ASSERT_TRUE(saved);
    EXPECT_FALSE(saved->displays.last().enabled);
    EXPECT_EQ(saved->displays.first().mode.pixelSize, QSize(1280, 720));
    EXPECT_EQ(first.pixelSize, QSize(1280, 720));
    EXPECT_FALSE(manager.applied()->physicalDisplays.last().currentMode);
}

TEST_F(DisplayRuntimeTest, LastDisplayPowerOffRequestsLockBeforePowerChange)
{
    const auto firstId = manager.applied()->physicalDisplays.first().sessionId;
    const auto secondId = manager.applied()->physicalDisplays.last().sessionId;
    int requests = 0;
    runtime->lockRequested = [&] {
        ++requests;
        EXPECT_FALSE(runtime->isPoweredOff(secondId));
        runtime->setSecureScanout(true);
    };
    ASSERT_TRUE(runtime->setPower({firstId}, display::NativeDisplayDelegate::PowerState::Off)
                        .succeeded);
    EXPECT_EQ(requests, 0);
    ASSERT_TRUE(runtime->setPower({secondId}, display::NativeDisplayDelegate::PowerState::Off)
                        .succeeded);
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(runtime->isSecureScanout());
    ASSERT_TRUE(runtime->setPower({}, display::NativeDisplayDelegate::PowerState::On).succeeded);
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(runtime->isSecureScanout());
    ASSERT_TRUE(runtime->setPower({}, display::NativeDisplayDelegate::PowerState::Off).succeeded);
    EXPECT_EQ(requests, 2);
    input.onPointerMotion({}, QPointF(1, 0));
    EXPECT_FALSE(runtime->isPoweredOff(firstId));
    EXPECT_FALSE(runtime->isPoweredOff(secondId));
    EXPECT_TRUE(runtime->isSecureScanout());
    EXPECT_EQ(requests, 2);
}

TEST_F(DisplayRuntimeTest, GlobalPowerOffWakesOnPhysicalInputWithoutChangingLayout)
{
    const auto revision = manager.applied()->revision;
    const auto bounds = manager.desktopBounds();
    const auto firstId = manager.applied()->physicalDisplays.first().sessionId;
    const auto secondId = manager.applied()->physicalDisplays.last().sessionId;
    ASSERT_TRUE(runtime->setPower({}, display::NativeDisplayDelegate::PowerState::Off).succeeded);
    EXPECT_TRUE(runtime->isPoweredOff(firstId));
    EXPECT_TRUE(runtime->isPoweredOff(secondId));
    input.warpPointer(QPointF(20, 20));
    EXPECT_TRUE(runtime->isPoweredOff(firstId));
    input.onPointerMotion(input::Timestamp {}, QPointF(1, 0));
    EXPECT_FALSE(runtime->isPoweredOff(firstId));
    EXPECT_FALSE(runtime->isPoweredOff(secondId));
    EXPECT_EQ(manager.applied()->revision, revision);
    EXPECT_EQ(manager.desktopBounds(), bounds);
    ASSERT_TRUE(runtime->setPower({}, display::NativeDisplayDelegate::PowerState::Off).succeeded);
    input.onKey(input::Timestamp {}, 30, true);
    EXPECT_FALSE(runtime->isPoweredOff(firstId));
}

TEST_F(DisplayRuntimeTest, WakeDoesNotUndoAnExplicitlyPoweredOffLidDisplay)
{
    const auto firstId = manager.applied()->physicalDisplays.first().sessionId;
    const auto secondId = manager.applied()->physicalDisplays.last().sessionId;
    ASSERT_TRUE(runtime->setPower({firstId}, display::NativeDisplayDelegate::PowerState::Off)
                        .succeeded);
    ASSERT_TRUE(runtime->setPower({}, display::NativeDisplayDelegate::PowerState::Off).succeeded);
    input.onPointerMotion(input::Timestamp {}, QPointF(1, 0));
    EXPECT_TRUE(runtime->isPoweredOff(firstId));
    EXPECT_FALSE(runtime->isPoweredOff(secondId));
}

TEST_F(DisplayRuntimeTest, PublicationRejectsReentrantConfigurationWithoutReplacingState)
{
    bool rejected = false;
    QObject::connect(&manager, &display::DisplayManager::displayChangeStarted, &manager,
                     [this, &rejected] {
                         auto nested = *manager.applied();
                         nested.revision += 100;
                         display::DisplayError error;
                         rejected = !manager.installApplied(nested, &error);
                         EXPECT_EQ(error.code, display::DisplayErrorCode::Busy);
                     });
    ASSERT_TRUE(apply(request(1)).succeeded);
    EXPECT_TRUE(rejected);
    EXPECT_EQ(manager.applied()->revision, 2u);
    expectPublished();
}

TEST_F(DisplayRuntimeTest, HotplugRestoresSavedMirrorAfterPrimaryRemovalAndZeroOutputs)
{
    auto mirror = request(1, 180);
    mirror.primary = mirror.displays.last().display;
    mirror.placements.clear();
    mirror.mirrorGroups = {{mirror.primary, mirror.displays.first().display}};
    ASSERT_TRUE(apply(mirror).succeeded);
    const auto original = *manager.hardware();
    QFile saved(directory.filePath(QStringLiteral("display.json")));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    const auto bytes = saved.readAll();
    saved.close();
    QList<display::DisplaySnapshot> source = original.displays;
    display::DisplayTopologyCoordinator coordinator(
            &manager, delegate.get(), controller.get(),
            [&] {
                QList<compositor::Output *> models;
                for (const auto &physical : source) {
                    auto *model = physical.connectorName == first.name ? &first : &second;
                    model->displayId = physical.sessionId.value();
                    models.append(model);
                }
                runtime->replaceOutputs(models);
                return std::optional(display::HardwareSnapshot {
                        original.compositorInstanceId, original.seatId,
                        manager.hardware()->topologyGeneration + 1, source});
            },
            display::DisplayPreferences(saved.fileName()));
    const auto change = [&] {
        QSignalSpy done(&coordinator, &display::DisplayTopologyCoordinator::reconciled);
        coordinator.invalidate();
        ASSERT_TRUE(done.wait(2000));
        ASSERT_TRUE(done.last().first().toBool());
    };
    source.removeLast();
    change();
    ASSERT_EQ(manager.applied()->logicalDisplays.size(), 1);
    EXPECT_EQ(manager.primaryDisplay()->id, original.displays.first().sessionId);
    EXPECT_EQ(input.topology().size(), 1);
    source.clear();
    change();
    EXPECT_TRUE(manager.applied()->logicalDisplays.isEmpty());
    EXPECT_TRUE(input.topology().isEmpty());
    EXPECT_EQ(primaryGlobal, nullptr);
    source = original.displays;
    std::reverse(source.begin(), source.end());
    change();
    ASSERT_EQ(manager.applied()->logicalDisplays.size(), 1);
    EXPECT_EQ(manager.primaryDisplay()->id, mirror.primary);
    EXPECT_EQ(manager.primaryDisplay()->destinations, mirror.mirrorGroups.first());
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_EQ(saved.readAll(), bytes);
}

TEST_F(DisplayRuntimeTest, InvalidationCancelsQueuedCommitBeforeNativeExecution)
{
    int commits = 0;
    controller.reset();
    delegate = std::make_unique<display::CallbackNativeDisplayDelegate>(
            *manager.hardware(),
            [&](const auto &configuration, auto mode) {
                if (mode == display::NativeDisplayDelegate::ConfigureMode::Commit)
                    ++commits;
                else
                    QTimer::singleShot(0, delegate.get(), [&] {
                        delegate->invalidateSnapshots();
                    });
                return runtime->configure(configuration, mode);
            },
            nullptr);
    makeController(directory.filePath(QStringLiteral("display.json")));
    const auto revision = manager.applied()->revision;
    const auto result = apply(request(1));
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::StaleConfiguration);
    EXPECT_EQ(commits, 0);
    EXPECT_EQ(manager.applied()->revision, revision);
}

TEST_F(DisplayRuntimeTest, TopologyWaitsForActiveOperationAndRecoversAfterProbeFailure)
{
    const auto original = *manager.hardware();
    int calls = 0;
    display::DisplayTopologyCoordinator coordinator(
            &manager, delegate.get(), controller.get(),
            [&]() -> std::optional<display::HardwareSnapshot> {
                EXPECT_FALSE(controller->busy());
                if (++calls == 1)
                    return std::nullopt;
                auto hardware = original;
                hardware.topologyGeneration = manager.hardware()->topologyGeneration + 1;
                return hardware;
            },
            display::DisplayPreferences(directory.filePath(QStringLiteral("display.json"))));
    QSignalSpy operation(controller.get(),
                         &display::DisplayConfigurationController::operationFinished);
    QSignalSpy done(&coordinator, &display::DisplayTopologyCoordinator::reconciled);
    controller->apply(request(1), QStringLiteral("internal"), 0);
    coordinator.invalidate();
    ASSERT_TRUE(done.wait(2000));
    EXPECT_TRUE(done.last().first().toBool());
    EXPECT_EQ(calls, 2);
    ASSERT_GE(operation.size(), 2);
    EXPECT_FALSE(qvariant_cast<display::ConfigurationResult>(operation.first().at(1)).succeeded);
    EXPECT_FALSE(controller->busy());
    EXPECT_EQ(manager.hardware()->topologyGeneration, 2u);
}

TEST_F(DisplayRuntimeTest, StartGivesUnsavedDisplaysTheirRecommendedScale)
{
    auto hardware = *manager.hardware();
    for (auto &physical : hardware.displays) {
        // 13.3" at 1080p, 166 dpi.
        physical.connectionType = display::DisplayConnectionType::Internal;
        physical.physicalSizeMm = {294, 165};
    }
    display::DisplayTopologyCoordinator coordinator(
            &manager, delegate.get(), controller.get(),
            [&] {
                auto next = hardware;
                next.topologyGeneration = manager.hardware()->topologyGeneration + 1;
                return std::optional(next);
            },
            display::DisplayPreferences(directory.filePath(QStringLiteral("display.json"))));
    QSignalSpy done(&coordinator, &display::DisplayTopologyCoordinator::reconciled);
    // The layout initialize() made runs at 1; start() must not keep it.
    EXPECT_EQ(input.pointerPosition(), QPointF(960, 540));
    coordinator.start();
    ASSERT_TRUE(done.wait(2000));
    ASSERT_TRUE(done.last().first().toBool());
    ASSERT_EQ(manager.applied()->logicalDisplays.size(), 2);
    for (const auto &logical : manager.applied()->logicalDisplays)
        EXPECT_EQ(logical.scale.value(), 1.25);
    // Still the centre of the primary display, now 1536x864.
    EXPECT_EQ(input.pointerPosition(), QPointF(768, 432));
}

TEST_F(DisplayRuntimeTest, HotplugInsertionAndReorderingPreserveIdsAndRejectConcurrentRequests)
{
    const auto original = *manager.hardware();
    auto third = original.displays.first();
    third.sessionId = display::DisplayId(QStringLiteral("injected-third"));
    third.portKey = QStringLiteral("virtual/Virtual-3");
    third.persistentKey = QStringLiteral("virtual:virtual/Virtual-3");
    third.connectorName = QStringLiteral("Virtual-3");
    compositor::Output model;
    model.name = third.connectorName;
    model.displayId = third.sessionId.value();
    model.pixelSize = third.currentMode->pixelSize;
    bool rejected = false;
    display::DisplayTopologyCoordinator coordinator(
            &manager, delegate.get(), controller.get(),
            [&] {
                runtime->replaceOutputs({&second, &model, &first});
                QTimer::singleShot(0, &manager, [&] {
                    QSignalSpy completed(
                            controller.get(),
                            &display::DisplayConfigurationController::operationFinished);
                    controller->setPower({}, display::NativeDisplayDelegate::PowerState::Off);
                    ASSERT_EQ(completed.size(), 1);
                    rejected = qvariant_cast<display::ConfigurationResult>(completed.first().at(1))
                                       .error.code
                            == display::DisplayErrorCode::Busy;
                });
                return std::optional(display::HardwareSnapshot {
                        original.compositorInstanceId,
                        original.seatId,
                        manager.hardware()->topologyGeneration + 1,
                        {original.displays.last(), third, original.displays.first()}});
            },
            display::DisplayPreferences(directory.filePath(QStringLiteral("display.json"))));
    QSignalSpy done(&coordinator, &display::DisplayTopologyCoordinator::reconciled);
    coordinator.invalidate();
    ASSERT_TRUE(done.wait(2000));
    ASSERT_EQ(done.size(), 1);
    EXPECT_TRUE(done.first().first().toBool());
    EXPECT_TRUE(rejected);
    ASSERT_EQ(manager.applied()->logicalDisplays.size(), 3);
    EXPECT_NE(manager.display(original.displays.first().sessionId), nullptr);
    EXPECT_NE(manager.display(original.displays.last().sessionId), nullptr);
    EXPECT_NE(manager.display(third.sessionId), nullptr);
    EXPECT_EQ(first.displayId, original.displays.first().sessionId.value());
    EXPECT_EQ(input.topology().size(), 3);
}

TEST_F(DisplayRuntimeTest, LegacyNativePreferencesRestoreWithoutRewritingTheFile)
{
    auto legacyHardware = *manager.hardware();
    for (auto &physical : legacyHardware.displays) {
        physical.persistentKey = QStringLiteral("session:") + physical.model;
        physical.portKey = QStringLiteral("port:") + physical.connectorName;
    }
    const QString path = directory.filePath(QStringLiteral("legacy.json"));
    display::DisplayPreferences preferences(path);
    ASSERT_TRUE(preferences.save(legacyHardware, request(2, 180)));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto profiles = root.value("topologies").toObject();
    const auto profile = profiles.begin().value();
    QStringList oldKeys;
    for (const auto &physical : legacyHardware.displays)
        oldKeys.append(physical.persistentKey + QLatin1Char('@') + physical.portKey);
    std::sort(oldKeys.begin(), oldKeys.end());
    root.insert("topologies", QJsonObject {{oldKeys.join(QLatin1Char('|')), profile}});
    const auto bytes = QJsonDocument(root).toJson();
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
    file.close();
    const auto restored = preferences.restore(*manager.hardware(), manager.applied()->revision);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->displays.first().mode.pixelSize, QSize(1280, 720));
    EXPECT_EQ(restored->displays.first().scale.numerator(), 180u);
    EXPECT_EQ(restored->primary, manager.hardware()->displays.first().sessionId);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), bytes);
}

TEST_F(DisplayRuntimeTest, InvalidationDuringPublicationDoesNotPersistTheCandidate)
{
    bool invalidated = false;
    QObject::connect(&manager, &display::DisplayManager::displaysChanged, &manager, [&] {
        if (!invalidated) {
            invalidated = true;
            delegate->invalidateSnapshots();
        }
    });
    const auto result = apply(request(1, 180));
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::StaleConfiguration);
    EXPECT_FALSE(QFile::exists(directory.filePath(QStringLiteral("display.json"))));
    EXPECT_FALSE(controller->busy());
}

TEST_F(DisplayRuntimeTest, ZeroOutputsParkMaximizedWindowsDuringPanelReservationRemoval)
{
    auto *client = window({100, 100, 300, 200});
    client->maximize();
    const auto parked = client->bounds();
    const auto previous = *manager.applied();
    manager.clearApplied(manager.hardware()->topologyGeneration);
    wm.setExclusiveZone(this, wm::ScreenEdge::Top, 24, first.name);
    wm.clearExclusiveZone(this);
    EXPECT_EQ(client->bounds(), parked);
    auto restored = previous;
    ++restored.revision;
    ASSERT_TRUE(manager.installApplied(restored));
    EXPECT_EQ(client->bounds(), parked);
    client->restore();
    EXPECT_EQ(client->bounds(), QRect(100, 100, 300, 200));
}

TEST_F(DisplayRuntimeTest, RecoveryFallbackPublishesSelectedModesAndPreservesSavedIntent)
{
    const auto savedRequest = request();
    ASSERT_TRUE(apply(savedRequest).succeeded);
    QFile preferences(directory.filePath(QStringLiteral("display.json")));
    ASSERT_TRUE(preferences.open(QIODevice::ReadOnly));
    const auto savedBytes = preferences.readAll();
    preferences.close();
    auto hardware = *manager.hardware();
    ++hardware.topologyGeneration;
    hardware.displays[0].connectionType = display::DisplayConnectionType::Internal;
    hardware.displays[1].connectionType = display::DisplayConnectionType::External;
    hardware.displays[1].baseConnectorKey = QStringLiteral("DP-1");
    QList<display::DisplayConfiguration> tested;
    QList<display::DisplayConfiguration> committed;
    runtime->setNativeConfigureForTesting([&](const auto &candidate, auto phase) {
        if (phase == display::NativeDisplayDelegate::ConfigureMode::Commit) {
            committed.append(candidate);
            return display::ConfigurationResult {true, false, {}, std::nullopt};
        }
        tested.append(candidate);
        if (candidate.displays[1].mode.pixelSize.width() > 1600)
            return display::ConfigurationResult {false,
                                                 false,
                                                 {display::DisplayErrorCode::TestFailed,
                                                  QStringLiteral("Injected combined link limit"),
                                                  {candidate.displays[1].display}},
                                                 std::nullopt};
        return display::ConfigurationResult {true, false, {}, std::nullopt};
    });
    display::DisplayTopologyCoordinator topology(
            &manager, delegate.get(), controller.get(),
            [&]() -> std::optional<display::HardwareSnapshot> {
                return hardware;
            },
            display::DisplayPreferences(directory.filePath(QStringLiteral("display.json"))));
    QSignalSpy operations(controller.get(),
                          &display::DisplayConfigurationController::operationFinished);
    QSignalSpy reconciled(&topology, &display::DisplayTopologyCoordinator::reconciled);
    topology.invalidate();
    ASSERT_TRUE(reconciled.wait(2000));
    ASSERT_TRUE(reconciled.first().first().toBool());
    ASSERT_EQ(tested.size(), 2);
    ASSERT_EQ(committed.size(), 1);
    EXPECT_EQ(committed.first().displays[0].mode.pixelSize, QSize(1920, 1080));
    EXPECT_EQ(committed.first().displays[1].mode.pixelSize, QSize(1600, 900));
    EXPECT_FALSE(committed.first().allowModeFallback);
    ASSERT_EQ(operations.size(), 1);
    const auto result = qvariant_cast<display::ConfigurationResult>(operations.first()[1]);
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.degraded);
    EXPECT_TRUE(result.error.message.contains(QStringLiteral("2 test rounds")));
    EXPECT_EQ(manager.applied()->physicalDisplays[1].currentMode->pixelSize, QSize(1600, 900));
    expectPublished();
    ASSERT_TRUE(preferences.open(QIODevice::ReadOnly));
    EXPECT_EQ(preferences.readAll(), savedBytes);
    auto intent = display::DisplayPreferences(preferences.fileName())
                          .restore(hardware, manager.applied()->revision);
    ASSERT_TRUE(intent);
    EXPECT_EQ(intent->displays[1].mode.pixelSize, QSize(1920, 1080));
}

TEST_F(DisplayRuntimeTest, ExhaustedCombinedModesUseOneBoundedSingleOutputRescue)
{
    auto hardware = *manager.hardware();
    for (auto &physical : hardware.displays)
        physical.connectionType = display::DisplayConnectionType::External;
    ASSERT_TRUE(replaceTestHardware(hardware));
    int tests = 0;
    runtime->setNativeConfigureForTesting([&](const auto &candidate, auto phase) {
        if (phase == display::NativeDisplayDelegate::ConfigureMode::Commit)
            return display::ConfigurationResult {true, false, {}, std::nullopt};
        ++tests;
        int active = 0;
        for (const auto &request : candidate.displays)
            active += request.enabled;
        return display::ConfigurationResult {active == 1,
                                             false,
                                             {active == 1 ? display::DisplayErrorCode::None
                                                          : display::DisplayErrorCode::TestFailed,
                                              QStringLiteral("Combined resource limit"),
                                              {}},
                                             std::nullopt};
    });
    QSignalSpy completed(controller.get(),
                         &display::DisplayConfigurationController::operationFinished);
    controller->apply(request(), QStringLiteral("topology"), -1);
    ASSERT_TRUE(completed.wait(2000));
    const auto result = qvariant_cast<display::ConfigurationResult>(completed.first()[1]);
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.degraded);
    EXPECT_EQ(tests, 8);
    EXPECT_EQ(manager.applied()->logicalDisplays.size(), 1);
    EXPECT_EQ(result.error.affectedDisplays.size(), 1);
    EXPECT_FALSE(QFile::exists(directory.filePath(QStringLiteral("display.json"))));
}

TEST_F(DisplayRuntimeTest, StableDevicePathReadsLegacyCardPortPreferencesWithoutRewriting)
{
    auto oldHardware = *manager.hardware();
    for (int i = 0; i < oldHardware.displays.size(); ++i) {
        auto &physical = oldHardware.displays[i];
        physical.devicePath = QStringLiteral("/dev/dri/card%1").arg(i);
        physical.portKey = physical.devicePath + QStringLiteral("/DP-1");
        physical.persistentKey = QStringLiteral("port:") + physical.portKey;
    }
    display::DisplayPreferences preferences(directory.filePath(QStringLiteral("display.json")));
    ASSERT_TRUE(preferences.save(oldHardware, request(1, 150)));
    QFile file(directory.filePath(QStringLiteral("display.json")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    file.close();
    auto hardware = oldHardware;
    for (int i = 0; i < hardware.displays.size(); ++i) {
        auto &physical = hardware.displays[i];
        physical.portKey = QStringLiteral("/sys/devices/pci%1/DP-1").arg(i);
        physical.persistentKey = QStringLiteral("port:") + physical.portKey;
    }
    const auto restored = preferences.restore(hardware, manager.applied()->revision);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->displays.first().mode.pixelSize, QSize(1600, 900));
    EXPECT_EQ(restored->displays.first().scale.numerator(), 150);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), bytes);
}

TEST_F(DisplayRuntimeTest, FailedNativeCompensationUnpublishesAndKeepsSavedIntent)
{
    ASSERT_TRUE(apply(request()).succeeded);
    QFile file(directory.filePath(QStringLiteral("display.json")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto saved = file.readAll();
    file.close();
    runtime->setNativeConfigureForTesting([](const auto &, auto phase) {
        if (phase == display::NativeDisplayDelegate::ConfigureMode::TestOnly)
            return display::ConfigurationResult {true, false, {}, std::nullopt};
        return display::ConfigurationResult {false,
                                             false,
                                             {display::DisplayErrorCode::RevertFailed,
                                              QStringLiteral("A device compensation failed"),
                                              {}},
                                             std::nullopt};
    });
    const auto result = apply(request(1));
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::RevertFailed);
    EXPECT_FALSE(manager.applied());
    EXPECT_EQ(primaryGlobal, nullptr);
    EXPECT_TRUE(scene.size().isEmpty());
    EXPECT_FALSE(controller->busy());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), saved);
}

TEST_F(DisplayRuntimeTest, ExactUserRequestCannotOptIntoRecoveryFallback)
{
    int tests = 0;
    runtime->setNativeConfigureForTesting([&](const auto &, auto) {
        ++tests;
        return display::ConfigurationResult {
                false,
                false,
                {display::DisplayErrorCode::TestFailed, QStringLiteral("Rejected exact mode"), {}},
                std::nullopt};
    });
    auto requested = request();
    requested.allowModeFallback = true;
    const auto result = apply(requested);
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.degraded);
    EXPECT_EQ(tests, 1);
    EXPECT_FALSE(QFile::exists(directory.filePath(QStringLiteral("display.json"))));
}

TEST_F(DisplayRuntimeTest, RecoveryFallbackStopsOnAllocationFailureAndBoundsTests)
{
    auto hardware = *manager.hardware();
    for (auto &physical : hardware.displays)
        physical.connectionType = display::DisplayConnectionType::External;
    ASSERT_TRUE(replaceTestHardware(hardware));
    auto requested = request();
    requested.allowModeFallback = true;
    int tests = 0;
    auto code = display::DisplayErrorCode::ResourceExhausted;
    runtime->setNativeConfigureForTesting([&](const auto &, auto) {
        ++tests;
        return display::ConfigurationResult {
                false, false, {code, QStringLiteral("Injected failure"), {}}, std::nullopt};
    });
    auto result =
            runtime->configure(requested, display::NativeDisplayDelegate::ConfigureMode::TestOnly);
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(tests, 1);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::ResourceExhausted);
    code = display::DisplayErrorCode::TestFailed;
    tests = 0;
    result = runtime->configure(requested, display::NativeDisplayDelegate::ConfigureMode::TestOnly);
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(tests, 8);
    EXPECT_FALSE(result.selected);
}

TEST_F(DisplayRuntimeTest, RecoveryInvalidationAfterSelectedTestPreventsCommit)
{
    auto hardware = *manager.hardware();
    hardware.displays[1].connectionType = display::DisplayConnectionType::External;
    ASSERT_TRUE(replaceTestHardware(hardware));
    int tests = 0, commits = 0;
    runtime->setNativeConfigureForTesting([&](const auto &candidate, auto phase) {
        if (phase == display::NativeDisplayDelegate::ConfigureMode::Commit) {
            ++commits;
            return display::ConfigurationResult {true, false, {}, std::nullopt};
        }
        ++tests;
        if (candidate.displays[1].mode.pixelSize.width() > 1600)
            return display::ConfigurationResult {false,
                                                 false,
                                                 {display::DisplayErrorCode::TestFailed,
                                                  QStringLiteral("Link limit"),
                                                  {candidate.displays[1].display}},
                                                 std::nullopt};
        delegate->invalidateSnapshots();
        return display::ConfigurationResult {true, false, {}, std::nullopt};
    });
    const auto revision = manager.applied()->revision;
    QSignalSpy operations(controller.get(),
                          &display::DisplayConfigurationController::operationFinished);
    controller->apply(request(), QStringLiteral("topology"), -1);
    ASSERT_TRUE(operations.wait(2000));
    const auto result = qvariant_cast<display::ConfigurationResult>(operations.first()[1]);
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::StaleConfiguration);
    EXPECT_EQ(tests, 2);
    EXPECT_EQ(commits, 0);
    EXPECT_EQ(manager.applied()->revision, revision);
}

TEST_F(DisplayRuntimeTest, RecoveryInvalidationDuringPublicationCannotReportSuccess)
{
    auto candidate = request();
    auto connection = QObject::connect(&manager, &display::DisplayManager::displaysChanged,
                                       delegate.get(), [&] {
                                           delegate->invalidateSnapshots();
                                       });
    QSignalSpy operations(controller.get(),
                          &display::DisplayConfigurationController::operationFinished);
    controller->apply(candidate, QStringLiteral("topology"), -1);
    ASSERT_TRUE(operations.wait(2000));
    const auto result = qvariant_cast<display::ConfigurationResult>(operations.first()[1]);
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::StaleConfiguration);
    EXPECT_FALSE(controller->busy());
    EXPECT_FALSE(QFile::exists(directory.filePath(QStringLiteral("display.json"))));
    QObject::disconnect(connection);
}

TEST_F(DisplayRuntimeTest, HardwareReplacementDuringFallbackStopsFurtherNativeTests)
{
    auto hardware = *manager.hardware();
    hardware.displays[1].connectionType = display::DisplayConnectionType::External;
    ASSERT_TRUE(replaceTestHardware(hardware));
    int tests = 0;
    runtime->setNativeConfigureForTesting([&](const auto &, auto) {
        ++tests;
        auto replacement = *manager.hardware();
        ++replacement.topologyGeneration;
        EXPECT_TRUE(manager.installHardware(replacement));
        return display::ConfigurationResult {
                false,
                false,
                {display::DisplayErrorCode::TestFailed, QStringLiteral("Hotplug during ioctl"), {}},
                std::nullopt};
    });
    auto candidate = request();
    candidate.allowModeFallback = true;
    const auto result =
            runtime->configure(candidate, display::NativeDisplayDelegate::ConfigureMode::TestOnly);
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::StaleConfiguration);
    EXPECT_EQ(tests, 1);
    EXPECT_FALSE(result.selected);
}

TEST_F(DisplayRuntimeTest, RecoveryCommitFailureDoesNotPublishOrRetryLowerModes)
{
    auto hardware = *manager.hardware();
    hardware.displays[1].connectionType = display::DisplayConnectionType::External;
    ASSERT_TRUE(replaceTestHardware(hardware));
    int tests = 0, commits = 0;
    runtime->setNativeConfigureForTesting([&](const auto &candidate, auto phase) {
        if (phase == display::NativeDisplayDelegate::ConfigureMode::Commit) {
            ++commits;
            return display::ConfigurationResult {false,
                                                 false,
                                                 {display::DisplayErrorCode::CommitFailed,
                                                  QStringLiteral("Commit rejected"),
                                                  {}},
                                                 std::nullopt};
        }
        ++tests;
        const bool accepted = candidate.displays[1].mode.pixelSize.width() <= 1600;
        return display::ConfigurationResult {
                accepted,
                false,
                {accepted ? display::DisplayErrorCode::None : display::DisplayErrorCode::TestFailed,
                 QStringLiteral("Combined test"),
                 {candidate.displays[1].display}},
                std::nullopt};
    });
    const auto revision = manager.applied()->revision;
    QSignalSpy operations(controller.get(),
                          &display::DisplayConfigurationController::operationFinished);
    controller->apply(request(), QStringLiteral("topology"), -1);
    ASSERT_TRUE(operations.wait(2000));
    const auto result = qvariant_cast<display::ConfigurationResult>(operations.first()[1]);
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.error.code, display::DisplayErrorCode::CommitFailed);
    EXPECT_EQ(tests, 2);
    EXPECT_EQ(commits, 1);
    EXPECT_EQ(manager.applied()->revision, revision);
    EXPECT_FALSE(QFile::exists(directory.filePath(QStringLiteral("display.json"))));
}

TEST(DisplayIdentity, ReorderPortMigrationDuplicatesMissingEdidAndVirtualModes)
{
    display::DisplayIdentity identity;
    display::DisplaySnapshot a;
    a.portKey = QStringLiteral("card/DP-1");
    a.edidKey = QStringLiteral("vendor-product-serial");
    a.connectionType = display::DisplayConnectionType::External;
    auto b = a;
    b.portKey = QStringLiteral("card/DP-2");
    QList<display::DisplaySnapshot> pair {a, b};
    identity.assign(pair);
    const auto firstId = pair.first().sessionId;
    const auto firstKey = pair.first().persistentKey;
    const auto secondId = pair.last().sessionId;
    EXPECT_NE(firstId, secondId);
    EXPECT_NE(firstKey, pair.last().persistentKey);
    std::reverse(pair.begin(), pair.end());
    identity.assign(pair);
    EXPECT_EQ(pair.first().sessionId, firstId);
    pair.removeLast();
    identity.assign(pair);
    EXPECT_EQ(pair.first().persistentKey, firstKey);
    display::DisplayIdentity restarted;
    restarted.rememberPersistentKeys({firstKey});
    restarted.assign(pair);
    EXPECT_EQ(pair.first().persistentKey, firstKey);
    display::DisplayIdentity unique;
    pair = {a};
    unique.assign(pair);
    const auto portableId = pair.first().sessionId;
    const auto portableKey = pair.first().persistentKey;
    pair.first().portKey = QStringLiteral("card/HDMI-A-1");
    unique.assign(pair);
    EXPECT_EQ(pair.first().sessionId, portableId);
    EXPECT_EQ(pair.first().persistentKey, portableKey);
    pair.first().edidKey.clear();
    unique.assign(pair);
    const auto noEdid = pair.first().sessionId;
    EXPECT_NE(noEdid, portableId);
    unique.assign(pair);
    EXPECT_EQ(pair.first().sessionId, noEdid);
    pair.first().connectionType = display::DisplayConnectionType::Virtual;
    unique.assign(pair);
    const auto virtualId = pair.first().sessionId;
    pair.first().edidKey = QStringLiteral("changed-mode-dependent-edid");
    unique.assign(pair);
    EXPECT_EQ(pair.first().sessionId, virtualId);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST_F(DisplayRuntimeTest, SecureBarrierUsesPhysicalDisplayIdsForLockAndUnlock)
{
    session::SecureContainer secure(scene.root()->addChild(std::make_unique<scene::Node>()));
    session::SessionController session(&secure);
    session.setDisplayConfiguration(*manager.applied());
    ASSERT_NE(first.displayId, first.name);
    ASSERT_TRUE(session.requestLock());
    session.notifyOutputPresented(first.name);
    EXPECT_EQ(session.state(), session::SessionState::Locking);
    session.notifyOutputPresented(first.displayId);
    EXPECT_EQ(session.state(), session::SessionState::Locking);
    session.notifyOutputPresented(second.displayId);
    ASSERT_EQ(session.state(), session::SessionState::Locked);
    ASSERT_TRUE(session.requestUnlock());
    session.notifyOutputPresented(second.displayId);
    EXPECT_TRUE(session.isBlocked());
    session.notifyOutputPresented(first.displayId);
    EXPECT_EQ(session.state(), session::SessionState::Active);
}

TEST_F(DisplayRuntimeTest, MirroredSecureBarrierWaitsForEveryPhysicalDestination)
{
    auto mirror = request(1, 180);
    mirror.placements.clear();
    mirror.mirrorGroups = {{mirror.primary, mirror.displays.last().display}};
    ASSERT_TRUE(apply(mirror).succeeded);
    ASSERT_EQ(compositor.outputViews().size(), 1);
    session::SecureContainer secure(scene.root()->addChild(std::make_unique<scene::Node>()));
    session::SessionController session(&secure);
    session.setDisplayConfiguration(*manager.applied());
    ASSERT_EQ(secure.outputs().size(), 2);
    ASSERT_TRUE(session.requestLock());
    session.notifyOutputPresented(first.displayId);
    EXPECT_EQ(session.state(), session::SessionState::Locking);
    session.notifyOutputPresented(second.displayId);
    EXPECT_EQ(session.state(), session::SessionState::Locked);
}
