// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Show desktop: which windows move aside, how far, and what brings them back.
// No DRM, no GL, no Wayland client.

#include <gtest/gtest.h>

#include <linux/input-event-codes.h>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "shm_test_util.h"
#include "wm/activation.h"
#include "wm/show_desktop_controller.h"
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
constexpr int kSliver = ShowDesktopController::kSliverWidth;

class ShowDesktopTest : public testing::Test
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
    ShowDesktopController *showDesktop = manager.showDesktop();

    std::vector<std::unique_ptr<compositor::Surface>> surfaces;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 22);
    Window *desktop = nullptr;
    double now = 1000;

    void SetUp() override
    {
        scene.setSize(kOutput.size());
        manager.setOutputBounds(kOutput);
        input.setBounds(kOutput.size());
        input.setEventSink(&manager);
        desktop = makeWindow(kOutput);
        desktop->setRole(WindowRole::Desktop);
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

    // Plays the running slide to its end.
    void finishSlide()
    {
        showDesktop->advance(now);
        now += ShowDesktopController::kSlideDurationMs;
        showDesktop->advance(now);
        now += 1000;
    }

    void click(const QPointF &location)
    {
        manager.onPointerButton(at(0), location, BTN_LEFT, true);
        manager.onPointerButton(at(0), location, BTN_LEFT, false);
    }
};

TEST_F(ShowDesktopTest, WindowsGoToTheNearerSideLeavingASliver)
{
    Window *left = makeWindow(QRect(100, 100, 300, 200));
    Window *right = makeWindow(QRect(600, 300, 300, 200));
    showDesktop->enter();
    finishSlide();

    EXPECT_TRUE(showDesktop->isActive());
    EXPECT_EQ(left->bounds(), QRect(100, 100, 300, 200));
    EXPECT_EQ(left->presentationOffset(), QPoint(kSliver - 400, 0));
    EXPECT_EQ(right->presentationOffset(), QPoint(1000 - kSliver - 600, 0));
    EXPECT_EQ(desktop->presentationOffset(), QPoint());
    // The desktop has the keyboard meanwhile.
    EXPECT_EQ(manager.activation()->activeWindow(), desktop);
}

TEST_F(ShowDesktopTest, MovesHalfwayMidSlideAndBackWhenToggledAgain)
{
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    showDesktop->enter();
    showDesktop->advance(now);
    showDesktop->advance(now + ShowDesktopController::kSlideDurationMs / 2);
    const int halfway = window->presentationOffset().x();
    EXPECT_LT(halfway, 0);
    EXPECT_GT(halfway, kSliver - 400);

    showDesktop->toggle();
    EXPECT_FALSE(showDesktop->isActive());
    now += ShowDesktopController::kSlideDurationMs;
    finishSlide();
    EXPECT_EQ(window->presentationOffset(), QPoint());
    EXPECT_FALSE(showDesktop->isMovedAside(window));
}

TEST_F(ShowDesktopTest, LeavesShellMinimizedAndHiddenWindowsAlone)
{
    Window *dock = makeWindow(QRect(200, 720, 600, 75));
    dock->setRole(WindowRole::Dock);
    Window *minimized = makeWindow(QRect(100, 100, 300, 200));
    minimized->minimize();
    Window *hidden = makeWindow(QRect(100, 100, 300, 200));
    hidden->setHidden(true);
    Window *window = makeWindow(QRect(100, 100, 300, 200));
    showDesktop->enter();
    finishSlide();

    EXPECT_TRUE(showDesktop->isMovedAside(window));
    EXPECT_FALSE(showDesktop->isMovedAside(dock));
    EXPECT_FALSE(showDesktop->isMovedAside(minimized));
    EXPECT_FALSE(showDesktop->isMovedAside(hidden));
    EXPECT_EQ(dock->presentationOffset(), QPoint());
}

TEST_F(ShowDesktopTest, NothingToMoveLeavesTheModeOff)
{
    showDesktop->enter();
    EXPECT_FALSE(showDesktop->isActive());
}

TEST_F(ShowDesktopTest, ActivatingAnApplicationWindowBringsThemBack)
{
    Window *first = makeWindow(QRect(100, 100, 300, 200));
    Window *second = makeWindow(QRect(600, 300, 300, 200));
    showDesktop->enter();
    finishSlide();

    // Clicking the desktop keeps them aside.
    click(QPointF(500, 650));
    EXPECT_TRUE(showDesktop->isActive());

    manager.activation()->activate(first);
    EXPECT_FALSE(showDesktop->isActive());
    finishSlide();
    EXPECT_EQ(first->presentationOffset(), QPoint());
    EXPECT_EQ(second->presentationOffset(), QPoint());
}

TEST_F(ShowDesktopTest, ClickingASliverBringsThatWindowBackInFront)
{
    Window *first = makeWindow(QRect(100, 100, 300, 200));
    Window *second = makeWindow(QRect(150, 150, 300, 200));
    manager.activation()->activate(second);
    showDesktop->enter();
    finishSlide();

    // The first window's sliver, below the second's.
    click(QPointF(kSliver / 2, 120));
    EXPECT_FALSE(showDesktop->isActive());
    EXPECT_EQ(manager.activation()->activeWindow(), first);
}

TEST_F(ShowDesktopTest, ADialogGoesWithItsWindow)
{
    Window *window = makeWindow(QRect(500, 100, 400, 300));
    Window *dialog = makeWindow(QRect(300, 150, 200, 100));
    dialog->setTransientParent(window);
    showDesktop->enter();
    finishSlide();
    EXPECT_EQ(dialog->presentationOffset(), window->presentationOffset());
}

TEST_F(ShowDesktopTest, MinimizingOrClosingWhileAsideReleasesTheWindow)
{
    Window *minimized = makeWindow(QRect(100, 100, 300, 200));
    Window *closed = makeWindow(QRect(600, 300, 300, 200));
    showDesktop->enter();
    finishSlide();

    minimized->minimize();
    EXPECT_FALSE(showDesktop->isMovedAside(minimized));
    EXPECT_EQ(minimized->presentationOffset(), QPoint());
    manager.destroyWindow(closed);
    EXPECT_TRUE(showDesktop->isActive());
}

} // namespace
