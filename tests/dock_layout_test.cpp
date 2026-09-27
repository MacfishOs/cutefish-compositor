// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Dock auto-hide policy. Behaviour modelled on ash/shelf/shelf_layout_manager_unittest.cc,
// rewritten against the Cutefish abstractions. No DRM, no GL, no Wayland client.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "shm_test_util.h"
#include "wm/dock_layout_manager.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace cutefish;
using namespace cutefish::wm;

namespace {

input::Timestamp at(int ms)
{
    return input::Timestamp(std::chrono::milliseconds(ms));
}

constexpr QRect kOutput(0, 0, 1000, 800);
constexpr QRect kDockBounds(200, 720, 600, 75);

class DockLayoutTest : public testing::Test
{
protected:
    scene::Scene scene;
    scene::Node *surfaceContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    compositor::Compositor compositor {surfaceContainer};
    testsupport::FakeRenderer renderer;
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    WindowManager manager {&compositor, &seat};
    DockLayoutManager layout {&manager};

    std::vector<std::unique_ptr<compositor::Surface>> surfaces;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 20);
    Window *dock = nullptr;

    void SetUp() override
    {
        scene.setSize(kOutput.size());
        manager.setOutputBounds(kOutput);
        input.setBounds(kOutput.size());
        input.setEventSink(&manager);
        dock = makeWindow(kDockBounds);
        dock->setRole(WindowRole::Dock);
        dock->setFocusPolicy(FocusPolicy::NoFocus);
        // The pointer rests in the middle of the screen unless a test moves it.
        moveTo(QPointF(500, 400));
        // The dock slides in as it maps; every test starts with it in place.
        finishSlide();
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

    Window *makeWindow(const QRect &bounds)
    {
        Window *window =
                manager.createWindow(makeSurface(bounds.size()), {.requestedSize = bounds.size()});
        window->setBounds(bounds);
        return window;
    }

    void moveTo(const QPointF &location) { manager.onPointerMotion(at(0), location); }

    // Runs the event loop until the reveal delay has passed.
    void waitForReveal()
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < DockLayoutManager::kRevealDelayMs + 200) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
    }

    // Plays the slide out to its end.
    void finishSlide()
    {
        layout.advance(1000);
        layout.advance(1000 + DockLayoutManager::kSlideDurationMs);
    }

    bool isShown() const { return layout.autoHideState(dock) == DockAutoHideState::Shown; }
};

TEST_F(DockLayoutTest, NeverHidesStaysPutOverMaximizedWindows)
{
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    window->maximize();

    EXPECT_EQ(DockVisibilityState::Visible, layout.visibilityState(dock));
    EXPECT_TRUE(isShown());
    EXPECT_EQ(QPoint(), dock->presentationOffset());
    EXPECT_FALSE(layout.isSliding(dock));
}

TEST_F(DockLayoutTest, FullscreenWindowHidesTheDockWhateverTheSetting)
{
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    window->activate();
    window->setFullscreen(true);

    EXPECT_EQ(DockVisibilityState::AutoHide, layout.visibilityState(dock));
    EXPECT_FALSE(isShown());
    EXPECT_TRUE(layout.isSliding(dock));

    finishSlide();
    EXPECT_FALSE(layout.isSliding(dock));
    EXPECT_TRUE(dock->isPresentationHidden());
    // Fully past the bottom edge.
    EXPECT_EQ(kOutput.bottom() + 1, dock->node()->absoluteGeometry().top());

    window->setFullscreen(false);
    EXPECT_TRUE(isShown());
    EXPECT_FALSE(dock->isPresentationHidden());
    finishSlide();
    EXPECT_EQ(QPoint(), dock->presentationOffset());
}

TEST_F(DockLayoutTest, PanelOnlyGivesWayToFullscreen)
{
    Window *panel = makeWindow(QRect(0, 0, 1000, 30));
    panel->setRole(WindowRole::Panel);
    layout.setBehavior(DockAutoHideBehavior::Always);
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    window->activate();
    // The setting is the dock's: the panel stays.
    EXPECT_EQ(DockVisibilityState::Visible, layout.visibilityState(panel));
    EXPECT_FALSE(isShown());

    window->maximize();
    EXPECT_EQ(DockVisibilityState::Visible, layout.visibilityState(panel));

    window->setFullscreen(true);
    EXPECT_EQ(DockVisibilityState::AutoHide, layout.visibilityState(panel));
    EXPECT_EQ(DockAutoHideState::Hidden, layout.autoHideState(panel));
    finishSlide();
    EXPECT_TRUE(panel->isPresentationHidden());
    EXPECT_EQ(QPoint(0, -30), panel->presentationOffset());

    // The top edge brings it back over the fullscreen window.
    moveTo(QPointF(500, 2));
    waitForReveal();
    EXPECT_EQ(DockAutoHideState::Shown, layout.autoHideState(panel));

    window->setFullscreen(false);
    EXPECT_EQ(DockVisibilityState::Visible, layout.visibilityState(panel));
}

TEST_F(DockLayoutTest, DocksAndPanelsSlideInFromTheirEdgeAsTheyMap)
{
    Window *panel = makeWindow(QRect(0, 0, 1000, 30));
    panel->setRole(WindowRole::Panel);
    layout.update();

    // Parked past the top edge, then on its way in without the reveal delay.
    EXPECT_EQ(DockAutoHideState::Shown, layout.autoHideState(panel));
    EXPECT_TRUE(layout.isSliding(panel));
    EXPECT_FALSE(panel->isPresentationHidden());
    EXPECT_EQ(QPoint(0, -30), panel->presentationOffset());
    layout.advance(1000);
    layout.advance(1000 + DockLayoutManager::kSlideDurationMs / 2);
    EXPECT_LT(-30, panel->presentationOffset().y());
    EXPECT_GT(0, panel->presentationOffset().y());
    layout.advance(1000 + DockLayoutManager::kSlideDurationMs);
    EXPECT_FALSE(layout.isSliding(panel));
    EXPECT_EQ(QPoint(), panel->presentationOffset());
}

TEST_F(DockLayoutTest, DocksAndPanelsWaitForTheSessionToStart)
{
    layout.setSessionStarting(true);
    Window *panel = makeWindow(QRect(0, 0, 1000, 30));
    panel->setRole(WindowRole::Panel);
    layout.update();
    EXPECT_EQ(DockAutoHideState::Hidden, layout.autoHideState(panel));
    EXPECT_FALSE(layout.isSliding(panel));
    EXPECT_TRUE(panel->isPresentationHidden());
    EXPECT_EQ(QPoint(0, -30), panel->presentationOffset());

    // Released: in at once, without the reveal delay.
    layout.setSessionStarting(false);
    EXPECT_EQ(DockAutoHideState::Shown, layout.autoHideState(panel));
    EXPECT_TRUE(layout.isSliding(panel));
    EXPECT_FALSE(panel->isPresentationHidden());
    finishSlide();
    EXPECT_EQ(QPoint(), panel->presentationOffset());
}

TEST_F(DockLayoutTest, DockMappedOverAFullscreenWindowStaysParked)
{
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    window->activate();
    window->setFullscreen(true);
    Window *second = makeWindow(QRect(0, 720, 1000, 75));
    second->setRole(WindowRole::Dock);
    layout.update();

    EXPECT_EQ(DockAutoHideState::Hidden, layout.autoHideState(second));
    EXPECT_FALSE(layout.isSliding(second));
    EXPECT_TRUE(second->isPresentationHidden());
    EXPECT_EQ(QPoint(0, kOutput.bottom() + 1 - 720), second->presentationOffset());
}

TEST_F(DockLayoutTest, EndingTheSessionPutsDocksAndPanelsAwayUntilItCarriesOn)
{
    Window *panel = makeWindow(QRect(0, 0, 1000, 30));
    panel->setRole(WindowRole::Panel);
    // Even with nothing to make room for, and the pointer on the dock.
    moveTo(QPointF(500, kDockBounds.center().y()));

    layout.setSessionEnding(true);
    EXPECT_FALSE(isShown());
    EXPECT_EQ(DockAutoHideState::Hidden, layout.autoHideState(panel));
    finishSlide();
    EXPECT_TRUE(dock->isPresentationHidden());
    EXPECT_EQ(QPoint(0, -30), panel->presentationOffset());

    // Cancelled: both come straight back, without the reveal delay.
    layout.setSessionEnding(false);
    EXPECT_TRUE(isShown());
    EXPECT_EQ(DockAutoHideState::Shown, layout.autoHideState(panel));
    finishSlide();
    EXPECT_EQ(QPoint(), dock->presentationOffset());
    EXPECT_EQ(QPoint(), panel->presentationOffset());
}

TEST_F(DockLayoutTest, DialogOverFullscreenWindowKeepsTheDockAway)
{
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    window->setFullscreen(true);
    Window *dialog = manager.createWindow(makeSurface(QSize(200, 100)),
                                          {.requestedSize = QSize(200, 100),
                                           .transientParent = window});
    dialog->activate();

    EXPECT_EQ(WorkspaceState::Fullscreen, layout.workspaceStateFor(kOutput));
    EXPECT_FALSE(isShown());
}

TEST_F(DockLayoutTest, InactiveFullscreenWindowUnderAnotherDoesNotHide)
{
    Window *fullscreen = makeWindow(QRect(100, 100, 300, 200));
    fullscreen->setFullscreen(true);
    Window *front = makeWindow(QRect(100, 100, 300, 200));
    front->activate();

    EXPECT_EQ(WorkspaceState::Default, layout.workspaceStateFor(kOutput));
    EXPECT_TRUE(isShown());
}

TEST_F(DockLayoutTest, WhenObstructedFollowsMaximizedWindows)
{
    layout.setBehavior(DockAutoHideBehavior::WhenObstructed);
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    EXPECT_TRUE(isShown());

    window->maximize();
    EXPECT_EQ(DockVisibilityState::AutoHide, layout.visibilityState(dock));
    EXPECT_FALSE(isShown());

    window->restore();
    EXPECT_TRUE(isShown());

    window->maximize();
    EXPECT_FALSE(isShown());
    window->minimize();
    EXPECT_TRUE(isShown());
}

TEST_F(DockLayoutTest, AlwaysHideStaysShownWithNothingToMakeRoomFor)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    EXPECT_EQ(DockVisibilityState::AutoHide, layout.visibilityState(dock));
    EXPECT_TRUE(isShown());

    Window *window = makeWindow(QRect(100, 100, 300, 200));
    EXPECT_FALSE(isShown());

    manager.destroyWindow(window);
    // Coming back is delayed like any reveal.
    EXPECT_FALSE(isShown());
    waitForReveal();
    EXPECT_TRUE(isShown());
}

TEST_F(DockLayoutTest, PointerAtTheEdgeRevealsAfterTheDelay)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    finishSlide();
    ASSERT_FALSE(isShown());
    ASSERT_TRUE(dock->isPresentationHidden());

    moveTo(QPointF(500, 799));
    // Not yet: brushing the edge does not pull the dock out.
    EXPECT_FALSE(isShown());

    waitForReveal();
    EXPECT_TRUE(isShown());
    EXPECT_FALSE(dock->isPresentationHidden());
    EXPECT_TRUE(layout.isSliding(dock));
}

TEST_F(DockLayoutTest, LeavingTheEdgeBeforeTheDelayCancelsTheReveal)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    finishSlide();

    moveTo(QPointF(500, 799));
    moveTo(QPointF(500, 400));
    waitForReveal();
    EXPECT_FALSE(isShown());
}

TEST_F(DockLayoutTest, PointerMovingOntoTheDockDuringTheDelayStillReveals)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    finishSlide();

    moveTo(QPointF(500, 799));
    moveTo(QPointF(500, 750));
    waitForReveal();
    EXPECT_TRUE(isShown());
}

TEST_F(DockLayoutTest, LeavingTheDockHidesAtOnceAndTheGapCounts)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    finishSlide();
    moveTo(QPointF(500, 799));
    waitForReveal();
    ASSERT_TRUE(isShown());

    // Between the dock and the edge, and off the dock's span but on the edge.
    moveTo(QPointF(500, 797));
    EXPECT_TRUE(isShown());
    moveTo(QPointF(50, 798));
    EXPECT_TRUE(isShown());

    moveTo(QPointF(500, 700));
    EXPECT_FALSE(isShown());
    EXPECT_TRUE(layout.isSliding(dock));
}

TEST_F(DockLayoutTest, OpenMenuAndLauncherKeepTheDockShown)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    ASSERT_FALSE(isShown());

    Window *menu = manager.createWindow(makeSurface(QSize(100, 40)),
                                        {.type = WindowType::Popup, .transientParent = dock});
    waitForReveal();
    EXPECT_TRUE(isShown());
    manager.destroyWindow(menu);
    EXPECT_FALSE(isShown());

    Window *launcher = makeWindow(kOutput);
    launcher->setRole(WindowRole::Launcher);
    layout.update();
    waitForReveal();
    EXPECT_TRUE(isShown());
    manager.destroyWindow(launcher);
    EXPECT_FALSE(isShown());
}

TEST_F(DockLayoutTest, SlideEasesToTheTargetAndParksOffScreen)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    ASSERT_TRUE(layout.isSliding(dock));

    layout.advance(1000);
    EXPECT_EQ(QPoint(), dock->presentationOffset());
    layout.advance(1000 + DockLayoutManager::kSlideDurationMs / 2);
    const int half = dock->presentationOffset().y();
    EXPECT_GT(half, 0);
    EXPECT_LT(half, kOutput.bottom() + 1 - kDockBounds.top());
    EXPECT_FALSE(dock->isPresentationHidden());

    layout.advance(1000 + DockLayoutManager::kSlideDurationMs);
    EXPECT_EQ(QPoint(0, kOutput.bottom() + 1 - kDockBounds.top()), dock->presentationOffset());
    EXPECT_TRUE(dock->isPresentationHidden());
    EXPECT_FALSE(layout.isSliding(dock));
}

TEST_F(DockLayoutTest, ReversingMidSlideStartsFromWhereTheDockIs)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    layout.advance(1000);
    layout.advance(1000 + DockLayoutManager::kSlideDurationMs / 2);
    const QPoint midway = dock->presentationOffset();

    manager.destroyWindow(window);
    waitForReveal();
    ASSERT_TRUE(isShown());
    layout.advance(2000);
    EXPECT_EQ(midway, dock->presentationOffset());
    layout.advance(2000 + DockLayoutManager::kSlideDurationMs);
    EXPECT_EQ(QPoint(), dock->presentationOffset());
}

TEST_F(DockLayoutTest, HiddenDockFollowsItsOwnBoundsOffScreen)
{
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    finishSlide();

    dock->setBounds(QRect(200, 700, 600, 95));
    finishSlide();
    EXPECT_EQ(QPoint(0, kOutput.bottom() + 1 - 700), dock->presentationOffset());
    EXPECT_TRUE(dock->isPresentationHidden());
}

TEST_F(DockLayoutTest, SideDockLeavesThroughItsOwnEdge)
{
    dock->setBounds(QRect(5, 100, 75, 600));
    layout.setBehavior(DockAutoHideBehavior::Always);
    makeWindow(QRect(100, 100, 300, 200));
    finishSlide();
    EXPECT_EQ(QPoint(-80, 0), dock->presentationOffset());

    moveTo(QPointF(2, 400));
    waitForReveal();
    EXPECT_TRUE(isShown());
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
