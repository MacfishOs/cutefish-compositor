// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Desks, modelled on chromium/src/ash/wm/desks/desks_unittest.cc,
// desk_animation_impl_unittest.cc and wm_gesture_handler_unittest.cc,
// rewritten against the Cutefish window manager. No DRM, no GL.

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <deque>
#include <random>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/desk_switch_node.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "scene/slide_out_node.h"
#include "shm_test_util.h"
#include "wm/activation.h"
#include "wm/desks/desk.h"
#include "wm/desks/desk_animation.h"
#include "wm/desks/desk_preferences.h"
#include "wm/desks/desks_controller.h"
#include "wm/gestures/wm_gesture_handler.h"
#include "wm/mru_window_tracker.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

using namespace cutefish;
using namespace cutefish::wm;

namespace {

constexpr QRect kOutput(0, 0, 1000, 800);

class FrameTexture : public render::Texture
{
public:
    explicit FrameTexture(QSize size)
        : m_size(size)
    {
    }

    QSize size() const override { return m_size; }

private:
    QSize m_size;
};

class FrameRenderer : public testsupport::FakeRenderer
{
public:
    bool renderToTexture(std::unique_ptr<render::Texture> &texture, const QRect &bounds,
                         const std::function<void()> &paint) override
    {
        paint();
        texture = std::make_unique<FrameTexture>(bounds.size());
        return true;
    }
};

class DesksTest : public testing::Test
{
protected:
    int argc = 0;
    QCoreApplication application {argc, nullptr};
    scene::Scene scene;
    scene::Node *surfaceContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    compositor::Compositor compositor {surfaceContainer};
    FrameRenderer renderer;
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    WindowManager manager {&compositor, &seat};
    DesksController *desks = manager.desks();

    std::vector<std::unique_ptr<compositor::Surface>> surfaces;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 20);

    void SetUp() override
    {
        scene.setSize(kOutput.size());
        manager.setOutputBounds(kOutput);
        input.setBounds(kOutput.size());
        input.setEventSink(&manager);
    }

    void TearDown() override { seat.shutdown(); }

    compositor::Surface *makeSurface(const QSize &size)
    {
        surfaces.push_back(compositor.createSurface());
        compositor::Surface *surface = surfaces.back().get();
        auto buffer = pool->createBuffer(size, 0, size.width() * 4, render::PixelFormat::Bgrx8888,
                                         nullptr);
        surface->attach(buffer, {});
        surface->damage(QRect(QPoint(), size));
        surface->commit();
        compositor.updateTextures(renderer);
        return surface;
    }

    Window *makeWindow(const QString &applicationId = QStringLiteral("app"),
                       Window *parent = nullptr)
    {
        Window *window = manager.createWindow(makeSurface({300, 200}),
                                              {.requestedSize = {300, 200},
                                               .applicationId = applicationId,
                                               .transientParent = parent});
        manager.activation()->activate(window);
        return window;
    }

    Desk *desk(int index) const { return desks->deskAt(index); }

    void addDesks(int count)
    {
        for (int i = 0; i < count; ++i)
            ASSERT_NE(nullptr, desks->newDesk(DesksCreationRemovalSource::Keyboard));
    }

    Window *active() { return manager.activation()->activeWindow(); }

    void flush()
    {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
};

// With the switch animation drawn on a node of the scene.
class DeskAnimationTest : public DesksTest
{
protected:
    qint64 now = 1000;
    scene::DeskSwitchNode *node = static_cast<scene::DeskSwitchNode *>(
            surfaceContainer->addChild(std::make_unique<scene::DeskSwitchNode>([this] {
                return now;
            })));
    scene::SlideOutNode *slideOut = static_cast<scene::SlideOutNode *>(
            surfaceContainer->addChild(std::make_unique<scene::SlideOutNode>([this] {
                return now;
            })));
    int screenshots = 0;
    bool screenshotsFail = false;
    QStringList outputs {QStringLiteral("out")};

    void SetUp() override
    {
        DesksTest::SetUp();
        manager.setDeskOverlays({node, slideOut});
        desks->setSwitchNode(node);
        desks->setWindowMoveNode(slideOut);
        desks->setScreenshotter([this](const std::function<void(render::Renderer &)> &paint) {
            scene::DeskSwitchNode::Frames frames;
            if (screenshotsFail)
                return frames;
            ++screenshots;
            paint(renderer);
            for (const QString &output : outputs)
                frames[output] = std::make_unique<FrameTexture>(kOutput.size());
            return frames;
        });
    }

    void paint(const QString &output = QStringLiteral("out"))
    {
        renderer.calls.clear();
        renderer.setOutputContext({output, kOutput, kOutput.size(), 1, 0, 0, {}});
        node->render(renderer);
        slideOut->render(renderer);
        flush();
    }

    // Draws until the animation has landed.
    void finish()
    {
        paint();
        now += 1000;
        paint();
    }

    // An input event's time on the clock the switch node runs on.
    static input::Timestamp at(qint64 ms)
    {
        return input::Timestamp(std::chrono::milliseconds(ms));
    }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////
// The desks and their windows

TEST_F(DesksTest, ThereIsAlwaysOneActiveDeskAndAtMostEight)
{
    ASSERT_EQ(1u, desks->desks().size());
    EXPECT_TRUE(desk(0)->isActive());
    EXPECT_FALSE(desks->canRemoveDesks());
    addDesks(DesksController::kMaxDesks - 1);
    EXPECT_FALSE(desks->canCreateDesks());
    EXPECT_EQ(nullptr, desks->newDesk(DesksCreationRemovalSource::Keyboard));
    EXPECT_TRUE(desk(0)->isActive());
    EXPECT_FALSE(desk(1)->isActive());
    // Every desk is told apart for good.
    EXPECT_NE(desk(0)->id(), desk(1)->id());
    EXPECT_EQ(desk(3), desks->deskById(desk(3)->id()));
}

TEST_F(DesksTest, NewWindowsGoToTheActiveDeskAndLeaveTheScreenWithIt)
{
    addDesks(1);
    Window *window = makeWindow();
    EXPECT_EQ(desk(0), desks->deskOf(window));
    EXPECT_TRUE(desks->belongsToActiveDesk(window));

    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_TRUE(desk(1)->isActive());
    EXPECT_TRUE(window->isDeskHidden());
    EXPECT_FALSE(window->isVisible());
    EXPECT_TRUE(window->targetVisibility());
    EXPECT_FALSE(window->node()->isVisible());
    EXPECT_TRUE(window->rootSurface()->isHidden());
    // Hidden with its desk, not minimized and not hidden by the user.
    EXPECT_FALSE(window->state()->isMinimized());
    EXPECT_FALSE(window->isHidden());
    EXPECT_EQ(nullptr, active());

    Window *second = makeWindow();
    EXPECT_EQ(desk(1), desks->deskOf(second));
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_TRUE(window->isVisible());
    EXPECT_FALSE(second->isVisible());
    // The window last used on the desk comes back active.
    EXPECT_EQ(window, active());
}

TEST_F(DesksTest, ActivatingAWindowOnAnotherDeskSwitchesToIt)
{
    addDesks(1);
    Window *first = makeWindow();
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *second = makeWindow();
    ASSERT_EQ(second, active());

    // The dock or the application switcher asking for it.
    manager.activation()->activate(first);
    EXPECT_EQ(first, active());
    EXPECT_TRUE(desk(0)->isActive());
    EXPECT_TRUE(first->isVisible());
    EXPECT_FALSE(second->isVisible());
    EXPECT_FALSE(second->isActive());
}

TEST_F(DesksTest, TheApplicationSwitcherListsWindowsOfEveryDesk)
{
    addDesks(1);
    Window *first = makeWindow(QStringLiteral("first"));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *second = makeWindow(QStringLiteral("second"));
    const auto cycle = manager.mruWindowTracker()->buildWindowListForCycling();
    EXPECT_EQ((std::vector<Window *> {second, first}), cycle);
    EXPECT_TRUE(canIncludeWindowInMruList(first));
}

TEST_F(DesksTest, NextActivationStaysOnTheActiveDesk)
{
    addDesks(1);
    Window *first = makeWindow();
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *second = makeWindow();
    manager.destroyWindow(second);
    // Not the window of the other desk, which would take the user there.
    EXPECT_EQ(nullptr, active());
    EXPECT_TRUE(desk(1)->isActive());
    EXPECT_FALSE(first->isVisible());
}

TEST_F(DesksTest, DialogsGoWhereTheirParentsAre)
{
    addDesks(1);
    Window *parent = makeWindow();
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    // A dialog of a window on another desk starts out hidden with it.
    Window *dialog = manager.createWindow(makeSurface({100, 100}),
                                          {.requestedSize = {100, 100}, .transientParent = parent});
    EXPECT_EQ(desk(0), desks->deskOf(dialog));
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_TRUE(desk(1)->isActive());

    // Sending the parent along takes the dialog too.
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_TRUE(dialog->isVisible());
    desks->sendToDeskAtIndex(parent, 1);
    EXPECT_EQ(desk(1), desks->deskOf(dialog));
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_FALSE(parent->isVisible());

    // Left behind when the parent closes, it stays on that desk.
    manager.destroyWindow(parent);
    EXPECT_EQ(desk(1), desks->deskOf(dialog));
    EXPECT_FALSE(dialog->isVisible());
}

TEST_F(DesksTest, AWindowSentToAnotherDeskLeavesTheUserWhereTheyAre)
{
    addDesks(2);
    Window *stays = makeWindow();
    Window *sent = makeWindow();
    ASSERT_EQ(sent, active());
    desks->sendToDeskAtIndex(sent, 2);
    EXPECT_TRUE(desk(0)->isActive());
    EXPECT_EQ(desk(2), desks->deskOf(sent));
    EXPECT_FALSE(sent->isVisible());
    // Activation moves to what is left on the desk.
    EXPECT_EQ(stays, active());

    // A minimized window sent elsewhere comes back on its new desk.
    stays->minimize();
    desks->sendToDeskAtIndex(stays, 2);
    EXPECT_FALSE(stays->state()->isMinimized());
    desks->activateDesk(desk(2), DesksSwitchSource::IndexedDeskSwitchShortcut);
    EXPECT_TRUE(stays->isVisible());
    // It was the one in use, so it came in on top.
    EXPECT_EQ(stays, manager.windows().back());
}

TEST_F(DesksTest, RemovingADeskHandsItsWindowsToTheDeskOnItsLeft)
{
    addDesks(2);
    Window *left = makeWindow(QStringLiteral("left"));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *removed = makeWindow(QStringLiteral("removed"));
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    ASSERT_EQ(left, active());

    Desk *middle = desk(1);
    desks->removeDesk(middle, DesksCreationRemovalSource::Keyboard);
    ASSERT_EQ(2u, desks->desks().size());
    EXPECT_EQ(desk(0), desks->deskOf(removed));
    EXPECT_TRUE(removed->isVisible());
    // Below the windows already there, and last in the order they were used.
    const auto &stacking = manager.windows();
    EXPECT_LT(std::find(stacking.begin(), stacking.end(), removed),
              std::find(stacking.begin(), stacking.end(), left));
    EXPECT_EQ(removed, manager.mruWindowTracker()->buildMruWindowList().back());
    EXPECT_EQ(left, active());
}

TEST_F(DesksTest, RemovingTheActiveDeskGoesLeftOrRightFromTheFirst)
{
    addDesks(2);
    Window *first = makeWindow();
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *second = makeWindow();
    desks->removeDesk(desks->activeDesk(), DesksCreationRemovalSource::Keyboard);
    EXPECT_EQ(desk(0), desks->activeDesk());
    EXPECT_TRUE(first->isVisible());
    EXPECT_TRUE(second->isVisible());

    // The first desk has none on its left.
    desks->removeDesk(desks->activeDesk(), DesksCreationRemovalSource::Keyboard);
    ASSERT_EQ(1u, desks->desks().size());
    EXPECT_TRUE(desk(0)->isActive());
    EXPECT_EQ(desk(0), desks->deskOf(first));
    EXPECT_EQ(desk(0), desks->deskOf(second));
    EXPECT_TRUE(first->isVisible());
}

TEST_F(DesksTest, WindowsOnEveryDeskStayOnScreen)
{
    addDesks(1);
    Window *pinned = makeWindow();
    desks->setVisibleOnAllDesks(pinned, true);
    EXPECT_EQ(nullptr, desks->deskOf(pinned));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_TRUE(pinned->isVisible());

    // Back on a desk of its own, the one in front of the user.
    desks->setVisibleOnAllDesks(pinned, false);
    EXPECT_EQ(desk(1), desks->deskOf(pinned));

    // Always-on-top windows are on every desk as they are.
    Window *floating = makeWindow();
    floating->setAlwaysOnTop(true);
    EXPECT_EQ(nullptr, desks->deskOf(floating));
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_TRUE(floating->isVisible());
    EXPECT_FALSE(pinned->isVisible());
    floating->setAlwaysOnTop(false);
    EXPECT_EQ(desk(0), desks->deskOf(floating));
}

TEST_F(DesksTest, ShellWindowsAreOnNoDesk)
{
    addDesks(1);
    Window *dock = makeWindow();
    dock->setRole(WindowRole::Dock);
    EXPECT_EQ(nullptr, desks->deskOf(dock));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_TRUE(dock->isVisible());
}

TEST_F(DesksTest, MinimizedAndHiddenWindowsKeepTheirStateAcrossDesks)
{
    addDesks(1);
    Window *minimized = makeWindow();
    Window *hidden = makeWindow(QStringLiteral("other"));
    minimized->minimize();
    hidden->setHidden(true);
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_TRUE(minimized->state()->isMinimized());
    EXPECT_FALSE(minimized->isVisible());
    EXPECT_TRUE(hidden->isHidden());
    EXPECT_FALSE(hidden->isVisible());
    // Neither comes back as the desk's active window.
    EXPECT_EQ(nullptr, active());
}

TEST_F(DesksTest, DeskNamesAreTheUsersOrEmpty)
{
    struct Names : DesksObserver {
        int changes = 0;
        void onDeskNameChanged(const Desk *) override { ++changes; }
    } observer;
    desks->addObserver(&observer);
    EXPECT_TRUE(desk(0)->name().isEmpty());
    desks->setDeskName(desk(0), QStringLiteral(" Work "));
    EXPECT_EQ(QStringLiteral("Work"), desk(0)->name());
    EXPECT_TRUE(desk(0)->isNameSetByUser());
    desks->setDeskName(desk(0), QString());
    EXPECT_FALSE(desk(0)->isNameSetByUser());
    EXPECT_EQ(2, observer.changes);
    Desk *named = desks->newDesk(DesksCreationRemovalSource::Api, QStringLiteral("Play"));
    EXPECT_EQ(QStringLiteral("Play"), named->name());
    desks->removeObserver(&observer);
}

TEST_F(DesksTest, PreferencesKeepTheDesksAcrossSessions)
{
    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    {
        DeskPreferences preferences(desks);
        addDesks(2);
        desks->setDeskName(desk(2), QStringLiteral("Music"));
        desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    }

    WindowManager restored(&compositor, &seat);
    DeskPreferences preferences(restored.desks());
    ASSERT_EQ(3u, restored.desks()->desks().size());
    EXPECT_EQ(1, restored.desks()->activeDeskIndex());
    EXPECT_TRUE(restored.desks()->deskAt(0)->name().isEmpty());
    EXPECT_EQ(QStringLiteral("Music"), restored.desks()->deskAt(2)->name());
}

////////////////////////////////////////////////////////////////////////////////
// Animations

TEST_F(DeskAnimationTest, ASwitchActivatesTheDeskBehindTheStartingFrame)
{
    addDesks(1);
    Window *first = makeWindow();
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    ASSERT_NE(nullptr, desks->animation());
    EXPECT_EQ(1, screenshots);
    EXPECT_TRUE(node->isActive());
    // Upstream activates the ending desk at the end of phase (1).
    EXPECT_TRUE(desk(1)->isActive());
    EXPECT_FALSE(first->isVisible());
    EXPECT_TRUE(desks->areDesksBeingModified());

    // The ending desk is copied at the next frame, then the strip slides.
    paint();
    EXPECT_TRUE(node->isAnimating());
    now += 1000;
    paint();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_FALSE(node->isActive());
    EXPECT_FALSE(desks->areDesksBeingModified());
}

TEST_F(DeskAnimationTest, TheNodeDrawsRightAboveTheDesksWindows)
{
    Window *window = makeWindow();
    Window *dock = makeWindow(QStringLiteral("dock"));
    dock->setRole(WindowRole::Dock);
    Window *floating = makeWindow(QStringLiteral("floating"));
    floating->setAlwaysOnTop(true);
    const auto &order = surfaceContainer->children();
    const auto at = [&order](scene::Node *node) {
        return std::find(order.begin(), order.end(), node) - order.begin();
    };
    EXPECT_LT(at(window->node()), at(node));
    EXPECT_LT(at(node), at(slideOut));
    EXPECT_LT(at(slideOut), at(floating->node()));
    EXPECT_LT(at(slideOut), at(dock->node()));
}

TEST_F(DeskAnimationTest, WithoutScreenshotsTheDeskSwitchesAtOnce)
{
    addDesks(1);
    screenshotsFail = true;
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    flush();
    EXPECT_TRUE(desk(1)->isActive());
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_FALSE(node->isActive());
}

TEST_F(DeskAnimationTest, AnOutputThatNeverDrawsDoesNotHoldTheSwitchUp)
{
    addDesks(1);
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    ASSERT_NE(nullptr, desks->animation());
    // No frame is drawn; the timeout lands the switch.
    QTest::qWait(DeskAnimationBase::kEndingScreenshotTimeoutMs + 100);
    flush();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_TRUE(desk(1)->isActive());
    EXPECT_FALSE(node->isActive());
}

TEST_F(DeskAnimationTest, PressingAgainTakesTheAnimationOneDeskFurther)
{
    addDesks(2);
    desks->activateAdjacentDesk(false, DesksSwitchSource::DeskSwitchShortcut);
    paint();
    // A second press while the first still animates extends it.
    EXPECT_TRUE(desks->activateAdjacentDesk(false, DesksSwitchSource::DeskSwitchShortcut));
    EXPECT_EQ(2, desks->animation()->endingDeskIndex());
    EXPECT_EQ(desk(2), desks->activeDesk());
    finish();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_EQ(2, desks->activeDeskIndex());
    // One starting frame for the whole run.
    EXPECT_EQ(1, screenshots);
}

TEST_F(DeskAnimationTest, AtTheEndTheDeskHitsTheWall)
{
    addDesks(1);
    EXPECT_TRUE(desks->activateAdjacentDesk(true, DesksSwitchSource::DeskSwitchShortcut));
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_TRUE(node->isActive());
    EXPECT_TRUE(desk(0)->isActive());
    finish();
    EXPECT_FALSE(node->isActive());
}

TEST_F(DeskAnimationTest, RemovingTheActiveDeskSlidesToTheOneThatTakesItsWindows)
{
    addDesks(1);
    desks->activateDesk(desk(1), DesksSwitchSource::IndexedDeskSwitchShortcut);
    finish();
    Window *window = makeWindow();
    const Desk *removed = desk(1);
    desks->removeDesk(desk(1), DesksCreationRemovalSource::Keyboard);
    ASSERT_NE(nullptr, desks->animation());
    // The desk goes only once the animation has landed.
    EXPECT_EQ(2u, desks->desks().size());
    EXPECT_TRUE(desk(0)->isActive());
    finish();
    ASSERT_EQ(1u, desks->desks().size());
    EXPECT_NE(removed, desk(0));
    EXPECT_EQ(desk(0), desks->deskOf(window));
    EXPECT_TRUE(window->isVisible());
    EXPECT_FALSE(node->isActive());
}

TEST_F(DeskAnimationTest, ASwipeFollowsTheFingersAndLandsOnTheNearestDesk)
{
    addDesks(2);
    WmGestureHandler gestures(desks);
    input.setGestureFilter(&gestures);
    // Finger travel in millimetres at `now`; the accelerated motion, far
    // larger, has no say.
    const auto swipe = [this](double mm) {
        const QPointF travel(mm / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
        input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel * 5, travel, 1, 0);
    };

    // Four fingers belong to the application.
    EXPECT_FALSE(gestures.claimGesture(input::Timestamp(), input::GestureType::Swipe, 4));
    EXPECT_FALSE(gestures.claimGesture(input::Timestamp(), input::GestureType::Pinch, 3));

    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    // Fingers moving left bring the desk on the right in.
    swipe(-1);
    EXPECT_EQ(nullptr, desks->animation());
    swipe(-2);
    ASSERT_NE(nullptr, desks->animation());
    EXPECT_EQ(desk(1), desks->activeDesk());
    EXPECT_EQ(desk(0), desks->chosenDesk());
    paint();
    // A desk's worth of travel: the strip is a little past the second desk,
    // and the desk beyond was activated for its frame.
    for (int i = 0; i < 6; ++i) {
        now += 8;
        swipe(-WmGestureHandler::kSwipeLengthPerDeskMm / 6);
    }
    EXPECT_EQ(desk(2), desks->activeDesk());
    EXPECT_EQ(desk(0), desks->chosenDesk());
    paint();
    // A fifth of a desk back towards the second, slowing down to hold there,
    // then let go.
    now += 8;
    swipe(WmGestureHandler::kSwipeLengthPerDeskMm / 5);
    for (int i = 0; i < 3; ++i) {
        now += 8;
        swipe(0.05);
    }
    now += 200;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    EXPECT_EQ(desk(1), desks->chosenDesk());
    finish();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_EQ(desk(1), desks->activeDesk());
    input.setGestureFilter(nullptr);
}

TEST_F(DeskAnimationTest, AShortFlickSwitchesDesksAndAShortHeldSwipeDoesNot)
{
    addDesks(1);
    WmGestureHandler gestures(desks);
    input.setGestureFilter(&gestures);
    for (const bool flick : {false, true}) {
        input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
        for (int i = 0; i < 4; ++i) {
            now += 8;
            const QPointF travel(-4 / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
            input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel, travel, 1, 0);
            paint();
        }
        // Held, the fingers slow down and stay before they lift.
        for (int i = 0; !flick && i < 3; ++i) {
            now += 8;
            const QPointF travel(-0.05 / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
            input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel, travel, 1, 0);
        }
        now += flick ? 8 : 200;
        input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
        finish();
        EXPECT_EQ(nullptr, desks->animation()) << flick;
        EXPECT_EQ(desk(flick ? 1 : 0), desks->activeDesk()) << flick;
    }
    input.setGestureFilter(nullptr);
}

TEST_F(DeskAnimationTest, ASwipeStartedDuringADragSwitchesNothing)
{
    addDesks(1);
    bool dragging = true;
    WmGestureHandler gestures(desks, [&dragging] {
        return dragging;
    });
    input.setGestureFilter(&gestures);
    const auto swipe = [this] {
        input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
        for (int i = 0; i < 4; ++i) {
            now += 8;
            const QPointF travel(-4 / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
            input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel, travel, 1, 0);
        }
    };

    // Not even once the drag is over: the rest of that swipe is ignored too.
    swipe();
    EXPECT_EQ(nullptr, desks->animation());
    dragging = false;
    now += 8;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    EXPECT_EQ(desk(0), desks->activeDesk());

    // The next swipe switches again.
    swipe();
    ASSERT_NE(nullptr, desks->animation());
    now += 8;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    finish();
    EXPECT_EQ(desk(1), desks->activeDesk());
    input.setGestureFilter(nullptr);
}

TEST_F(DeskAnimationTest, ASwipeWarmsUpAsItStartsUnlessItIsIgnored)
{
    bool dragging = false;
    WmGestureHandler gestures(desks, [&dragging] {
        return dragging;
    });
    int warmUps = 0;
    gestures.setWarmUp([&warmUps] {
        ++warmUps;
    });

    EXPECT_TRUE(gestures.claimGesture(input::Timestamp(), input::GestureType::Swipe, 3));
    EXPECT_EQ(1, warmUps);
    EXPECT_FALSE(gestures.claimGesture(input::Timestamp(), input::GestureType::Swipe, 4));
    EXPECT_EQ(1, warmUps);
    dragging = true;
    EXPECT_TRUE(gestures.claimGesture(input::Timestamp(), input::GestureType::Swipe, 3));
    EXPECT_EQ(1, warmUps);
}

TEST_F(DeskAnimationTest, ASingleDeskIgnoresTheSwipe)
{
    WmGestureHandler gestures(desks);
    input.setGestureFilter(&gestures);
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    for (const double mm : {10.0, -30.0}) {
        now += 8;
        const QPointF travel(mm / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
        input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel, travel, 1, 0);
        EXPECT_EQ(nullptr, desks->animation());
    }
    now += 8;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_FALSE(node->isActive());
    EXPECT_EQ(desk(0), desks->activeDesk());
    input.setGestureFilter(nullptr);
}

TEST_F(DeskAnimationTest, AtTheFirstDeskTheFingersPullAgainstTheEdge)
{
    addDesks(1);
    WmGestureHandler gestures(desks);
    input.setGestureFilter(&gestures);
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    const QPointF travel(10 / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
    input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel, travel, 1, 0);
    ASSERT_NE(nullptr, desks->animation());
    EXPECT_TRUE(node->isTracking());
    EXPECT_TRUE(node->reachedEdge());
    EXPECT_EQ(desk(0), desks->activeDesk());
    paint();
    now += 100;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    finish();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_FALSE(node->isActive());
    EXPECT_EQ(desk(0), desks->activeDesk());
    input.setGestureFilter(nullptr);
}

TEST_F(DeskAnimationTest, AnotherSwipeCatchesTheSettlingDesks)
{
    addDesks(1);
    WmGestureHandler gestures(desks);
    input.setGestureFilter(&gestures);
    const auto swipe = [this](double mm) {
        const QPointF travel(mm / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
        input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel, travel, 1, 0);
    };
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    swipe(-3);
    paint();
    swipe(-15);
    now += 200;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    paint();
    now += 50;
    paint();
    DeskAnimationBase *animation = desks->animation();
    ASSERT_NE(nullptr, animation);
    EXPECT_TRUE(node->isAnimating());

    // The same animation goes on under the fingers.
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    swipe(-3);
    EXPECT_EQ(animation, desks->animation());
    EXPECT_TRUE(node->isTracking());
    swipe(-40);
    now += 200;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    finish();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_EQ(desk(1), desks->activeDesk());
    input.setGestureFilter(nullptr);
}

TEST_F(DeskAnimationTest, ASwipeBackActivatesTheDeskAsTheFingersLift)
{
    addDesks(1);
    WmGestureHandler gestures(desks);
    input.setGestureFilter(&gestures);
    const auto swipe = [this](double mm) {
        const QPointF travel(mm / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
        input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel, travel, 1, 0);
    };
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    swipe(-3);
    paint();
    for (int i = 0; i < 4; ++i) {
        now += 8;
        swipe(-6);
    }
    now += 8;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    paint();
    now += 50;
    paint();
    EXPECT_EQ(desk(1), desks->activeDesk());

    // Flung back while the strip still settles: the first desk has its frame
    // in the strip already, and is active before the strip gets there.
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    for (int i = 0; i < 4; ++i) {
        now += 8;
        swipe(6);
    }
    EXPECT_EQ(desk(1), desks->chosenDesk());
    now += 8;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    EXPECT_TRUE(node->isAnimating());
    EXPECT_EQ(desk(0), desks->activeDesk());
    EXPECT_EQ(desk(0), desks->chosenDesk());
    finish();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_EQ(desk(0), desks->activeDesk());
    input.setGestureFilter(nullptr);
}

TEST_F(DeskAnimationTest, UpAndDownSwipesSwitchNothing)
{
    addDesks(1);
    WmGestureHandler gestures(desks);
    ASSERT_TRUE(gestures.claimGesture(input::Timestamp(), input::GestureType::Swipe, 3));
    gestures.onGestureUpdate(input::Timestamp(), {{}, {1, -50}});
    gestures.onGestureUpdate(input::Timestamp(), {{}, {-200, 0}});
    gestures.onGestureEnd(input::Timestamp(), false);
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_TRUE(desk(0)->isActive());
}

TEST_F(DeskAnimationTest, ActivatingAWindowOnAnotherDeskSlidesThereAndKeepsIt)
{
    addDesks(1);
    Window *first = makeWindow();
    desks->activateDesk(desk(1), DesksSwitchSource::IndexedDeskSwitchShortcut);
    finish();
    makeWindow();
    manager.activation()->activate(first);
    EXPECT_EQ(first, active());
    EXPECT_TRUE(desk(0)->isActive());
    ASSERT_NE(nullptr, desks->animation());
    finish();
    EXPECT_EQ(first, active());
    EXPECT_EQ(nullptr, desks->animation());
}

TEST_F(DeskAnimationTest, AWindowSentAwaySlidesOffTheScreen)
{
    addDesks(1);
    Window *window = makeWindow();
    desks->sendToDeskAtIndex(window, 1);
    EXPECT_TRUE(slideOut->isActive());
    EXPECT_FALSE(window->isVisible());
    now += scene::SlideOutNode::kDurationMs;
    paint();
    EXPECT_FALSE(slideOut->isActive());
}

TEST_F(DeskAnimationTest, ActivatingAWindowElsewhereMidSwitchLandsTheSwitchFirst)
{
    addDesks(2);
    Window *first = makeWindow();
    desks->activateDesk(desk(2), DesksSwitchSource::IndexedDeskSwitchShortcut);
    ASSERT_NE(nullptr, desks->animation());
    // Never an active window on a desk nobody sees.
    manager.activation()->activate(first);
    EXPECT_EQ(first, active());
    EXPECT_TRUE(desk(0)->isActive());
    EXPECT_TRUE(first->isVisible());
    finish();
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_TRUE(desk(0)->isActive());
    EXPECT_EQ(first, active());
}

TEST_F(DeskAnimationTest, SwitchingDuringARemovalStillRemovesTheDesk)
{
    addDesks(2);
    desks->activateDesk(desk(1), DesksSwitchSource::IndexedDeskSwitchShortcut);
    finish();
    Window *window = makeWindow();
    desks->removeDesk(desk(1), DesksCreationRemovalSource::Keyboard);
    ASSERT_NE(nullptr, desks->animation());
    desks->activateAdjacentDesk(false, DesksSwitchSource::DeskSwitchShortcut);
    ASSERT_EQ(2u, desks->desks().size());
    EXPECT_EQ(desk(0), desks->deskOf(window));
    finish();
    EXPECT_EQ(1, desks->activeDeskIndex());
}

TEST_F(DesksTest, AWindowBeingDraggedComesAlong)
{
    addDesks(1);
    Window *dragged = makeWindow();
    Window *left = makeWindow(QStringLiteral("other"));
    manager.activation()->activate(dragged);
    ASSERT_TRUE(manager.eventHandler()->beginMove(dragged, QPointF(150, 10)));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    EXPECT_EQ(desk(1), desks->deskOf(dragged));
    EXPECT_TRUE(dragged->isVisible());
    EXPECT_FALSE(left->isVisible());
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Success);
    EXPECT_TRUE(dragged->isVisible());
}

TEST_F(DeskAnimationTest, EveryOutputHasItsEndingFrameBeforeTheStripMoves)
{
    outputs = {QStringLiteral("a"), QStringLiteral("b")};
    addDesks(1);
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    paint(QStringLiteral("a"));
    // The displays move together, so the one that drew first waits.
    EXPECT_FALSE(node->isAnimating());
    paint(QStringLiteral("b"));
    EXPECT_TRUE(node->isAnimating());
    // An output plugged in meanwhile shows the live desk, not a black strip.
    renderer.setOutputContext({QStringLiteral("c"), kOutput, kOutput.size(), 1, 0, 0, {}});
    EXPECT_FALSE(node->occludesOutput(renderer));
    now += 1000;
    paint(QStringLiteral("a"));
    EXPECT_EQ(nullptr, desks->animation());
    EXPECT_TRUE(desk(1)->isActive());
}

////////////////////////////////////////////////////////////////////////////////
// Random runs

namespace {

// What must hold after anything the user or a client does, animation or not.
std::string checkInvariants(WindowManager &manager, DesksController *desks)
{
    const auto &list = desks->desks();
    if (list.empty() || list.size() > size_t(DesksController::kMaxDesks))
        return "desk count " + std::to_string(list.size());
    int active = 0;
    std::set<const Window *> seen;
    for (const auto &desk : list) {
        active += desk->isActive();
        for (const Window *window : desk->windows()) {
            if (!seen.insert(window).second)
                return "a window on two desks";
            if (desks->deskOf(window) != desk.get())
                return "a desk lists a window that is not on it";
        }
    }
    if (active != 1 || !desks->activeDesk() || !desks->activeDesk()->isActive())
        return "active desks " + std::to_string(active);
    for (Window *window : manager.windows()) {
        const Desk *desk = desks->deskOf(window);
        if (desk && desks->deskIndex(desk) < 0)
            return "a window on a removed desk";
        if (window->isDeskHidden() != (desk && !desk->isActive()))
            return "desk visibility of " + window->applicationId().toStdString();
        if (window->isVisible() != (window->targetVisibility() && !window->isDeskHidden()))
            return "visibility of " + window->applicationId().toStdString();
        const bool root = !window->transientParent() || !manager.hasWindow(window->transientParent());
        const bool deskWindow = root && window->role() == WindowRole::Normal && !window->isPopup()
                && !window->isAlwaysOnTop() && !window->isSystemDialog();
        if (deskWindow && !desk && !desks->isVisibleOnAllDesks(window))
            return "a desk window on no desk: " + window->applicationId().toStdString();
    }
    if (Window *activeWindow = manager.activation()->activeWindow();
        activeWindow && !activeWindow->isVisible())
        return "the active window is not on screen";
    return {};
}

} // namespace

TEST_F(DeskAnimationTest, RandomRunsKeepEveryInvariant)
{
    WmGestureHandler gestures(desks);
    input.setGestureFilter(&gestures);
    for (const unsigned seed : {1u, 7u, 42u, 2026u}) {
        std::mt19937 random(seed);
        const auto pick = [&random](int count) {
            return int(std::uniform_int_distribution<int>(0, count - 1)(random));
        };
        std::deque<std::string> history;
        int created = 0;
        const auto anyWindow = [&]() -> Window * {
            const auto &windows = manager.windows();
            return windows.empty() ? nullptr : windows[pick(int(windows.size()))];
        };
        bool swiping = false;
        for (int step = 0; step < 2500; ++step) {
            std::string op;
            switch (pick(22)) {
            case 0:
            case 1: {
                Window *parent = pick(4) == 0 ? anyWindow() : nullptr;
                const QString id = QStringLiteral("w%1").arg(++created);
                Window *window = manager.createWindow(
                        makeSurface({200, 150}),
                        {.requestedSize = {200, 150}, .applicationId = id, .transientParent = parent});
                if (pick(2))
                    manager.activation()->activate(window);
                op = "create " + id.toStdString() + (parent ? " dialog" : "");
                break;
            }
            case 2:
                if (Window *window = anyWindow()) {
                    op = "destroy " + window->applicationId().toStdString();
                    manager.destroyWindow(window);
                }
                break;
            case 3:
            case 4:
                if (Window *window = anyWindow()) {
                    op = "activate " + window->applicationId().toStdString();
                    manager.activation()->activate(window);
                }
                break;
            case 5:
                if (Window *window = anyWindow()) {
                    op = "minimize/restore " + window->applicationId().toStdString();
                    if (window->state()->isMinimized())
                        window->restore();
                    else
                        window->minimize();
                }
                break;
            case 6:
                if (Window *window = anyWindow()) {
                    op = "hide/unhide " + window->applicationId().toStdString();
                    window->setHidden(!window->isHidden());
                }
                break;
            case 7:
                if (Window *window = anyWindow()) {
                    op = "always on top " + window->applicationId().toStdString();
                    window->setAlwaysOnTop(!window->isAlwaysOnTop());
                }
                break;
            case 8:
                op = "new desk";
                if (Desk *desk = desks->newDesk(DesksCreationRemovalSource::Keyboard); desk && pick(2))
                    desks->activateDesk(desk, DesksSwitchSource::NewDeskShortcut);
                break;
            case 9:
                if (desks->canRemoveDesks() && !desks->areDesksBeingModified()) {
                    const int index = pick(int(desks->desks().size()));
                    op = "remove desk " + std::to_string(index);
                    desks->removeDesk(desk(index), DesksCreationRemovalSource::Keyboard);
                }
                break;
            case 10:
            case 11: {
                const bool left = pick(2);
                op = left ? "left" : "right";
                desks->activateAdjacentDesk(left, DesksSwitchSource::DeskSwitchShortcut);
                break;
            }
            case 12: {
                const int index = pick(int(desks->desks().size()));
                op = "activate desk " + std::to_string(index);
                if (!desks->areDesksBeingModified())
                    desks->activateDesk(desk(index), pick(2) ? DesksSwitchSource::ApiSwitch
                                                            : DesksSwitchSource::IndexedDeskSwitchShortcut);
                break;
            }
            case 13:
                if (Window *window = anyWindow()) {
                    const int index = pick(int(desks->desks().size()));
                    op = "send " + window->applicationId().toStdString() + " to "
                            + std::to_string(index);
                    desks->sendToDeskAtIndex(window, index);
                }
                break;
            case 14:
                if (Window *window = anyWindow()) {
                    op = "all desks " + window->applicationId().toStdString();
                    desks->setVisibleOnAllDesks(window, !desks->isVisibleOnAllDesks(window));
                }
                break;
            case 15:
                if (Window *window = anyWindow()) {
                    op = "rename";
                    if (Desk *target = desks->deskOf(window))
                        desks->setDeskName(target, pick(2) ? QStringLiteral("x") : QString());
                }
                break;
            case 16:
                op = "swipe";
                if (!swiping) {
                    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
                    swiping = true;
                }
                now += pick(20);
                {
                    const QPointF travel(pick(600) - 300, pick(60) - 30);
                    input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel,
                                                 travel, 1, 0);
                }
                break;
            case 17:
                op = "swipe end";
                if (swiping)
                    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, pick(5) == 0);
                swiping = false;
                break;
            case 18:
                op = "screenshots " + std::string(screenshotsFail ? "on" : "off");
                screenshotsFail = pick(6) == 0;
                break;
            default:
                now += pick(400);
                op = "paint";
                paint();
                break;
            }
            history.push_back(op);
            if (history.size() > 25)
                history.pop_front();
            const std::string broken = checkInvariants(manager, desks);
            if (!broken.empty()) {
                std::string trail;
                for (const auto &line : history)
                    trail += "\n  " + line;
                FAIL() << "seed " << seed << " step " << step << ": " << broken << trail;
            }
        }
        if (swiping)
            input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
        finish();
        now += 1000;
        finish();
        EXPECT_EQ(nullptr, desks->animation()) << "seed " << seed;
        EXPECT_EQ("", checkInvariants(manager, desks)) << "seed " << seed;
        // Next seed from a clean slate.
        while (!manager.windows().empty())
            manager.destroyWindow(manager.windows().back());
        while (desks->canRemoveDesks())
            desks->removeDesk(desk(int(desks->desks().size()) - 1),
                              DesksCreationRemovalSource::DesksRestore);
        // What a frame would do: closed windows' nodes leave the scene.
        surfaces.clear();
        scene.root()->collectRetired();
    }
    input.setGestureFilter(nullptr);
}
