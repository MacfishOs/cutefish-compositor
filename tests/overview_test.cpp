// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The window overview against the window manager: entering and leaving by the
// shortcut and by three fingers, the input it takes and leaves, clicking,
// closing and dragging windows, and the desk bar. No DRM, no GL.

#include <QCoreApplication>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include <gtest/gtest.h>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/overview_node.h"
#include "scene/scene.h"
#include "shm_test_util.h"
#include "wm/activation.h"
#include "wm/desks/desk.h"
#include "wm/desks/desks_controller.h"
#include "wm/gestures/wm_gesture_handler.h"
#include "wm/overview/overview_controller.h"
#include "wm/stacking.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

using namespace cutefish;
using namespace cutefish::wm;
using State = OverviewController::State;

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
        if (!texture || texture->size() != bounds.size())
            texture = std::make_unique<FrameTexture>(bounds.size());
        return true;
    }

    void drawBackgroundBlur(const QRect &bounds, int, const QRect &, const render::GlassParams &,
                            const QPainterPath &) override
    {
        blurs.append(bounds);
    }

    QList<QRect> blurs;
};

class OverviewTest : public testing::Test
{
protected:
    int argc = 0;
    QCoreApplication application {argc, nullptr};
    qint64 now = 1000;
    scene::Scene scene;
    scene::Node *surfaceContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    compositor::Compositor compositor {surfaceContainer};
    FrameRenderer renderer;
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    WindowManager manager {&compositor, &seat};
    DesksController *desks = manager.desks();
    OverviewController *overview = manager.overview();
    scene::OverviewNode *node = static_cast<scene::OverviewNode *>(
            surfaceContainer->addChild(std::make_unique<scene::OverviewNode>([this] {
                return now;
            })));
    scene::OverviewDeskBarNode *bar = static_cast<scene::OverviewDeskBarNode *>(
            surfaceContainer->addChild(std::make_unique<scene::OverviewDeskBarNode>(node)));

    std::vector<std::unique_ptr<compositor::Surface>> surfaces;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 20);

    void SetUp() override
    {
        scene.setSize(kOutput.size());
        manager.setOutputBounds(kOutput);
        manager.setDeskOverlays({node, bar});
        overview->setNodes(node, bar);
        overview->setNext(&manager);
        input.setBounds(kOutput.size());
        input.setEventSink(overview);
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
        Window *window = manager.createWindow(makeSurface(bounds.size()),
                                              {.requestedSize = bounds.size(),
                                               .applicationId = QStringLiteral("app")});
        window->setBounds(bounds);
        manager.activation()->activate(window);
        return window;
    }

    Window *active() { return manager.activation()->activeWindow(); }

    Desk *desk(int index) const { return desks->deskAt(index); }

    void flush()
    {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void paint()
    {
        renderer.calls.clear();
        renderer.setOutputContext({QStringLiteral("out"), kOutput, kOutput.size(), 1, 0, 0, {}});
        renderer.frameTime = double(now);
        surfaceContainer->render(renderer);
        flush();
    }

    // Draws until the overview has come to rest.
    void settle()
    {
        paint();
        now += 1500;
        paint();
        paint();
    }

    static input::Timestamp at(qint64 ms)
    {
        return input::Timestamp(std::chrono::milliseconds(ms));
    }

    void moveTo(const QPointF &location) { input.onPointerMotionLogical(at(now), location); }

    void click(const QPointF &location)
    {
        moveTo(location);
        input.onPointerButton(at(now), BTN_LEFT, true);
        input.onPointerButton(at(now), BTN_LEFT, false);
    }

    void key(uint32_t code)
    {
        input.onKey(at(now), code, true);
        input.onKey(at(now), code, false);
    }

    int indexOf(const Window *window) const
    {
        const auto &items = overview->items();
        for (size_t index = 0; index < items.size(); ++index) {
            if (items[index].window == window)
                return int(index);
        }
        return -1;
    }

    // Whether the last paint drew the window's client content anywhere.
    bool drewContentOf(const Window *window) const
    {
        const render::Texture *texture = window->rootSurface()->node()->texture();
        return texture
                && std::any_of(renderer.calls.begin(), renderer.calls.end(),
                               [texture](const auto &call) {
                                   return call.texture == texture;
                               });
    }

    QRectF placeOf(const Window *window) const
    {
        const int index = indexOf(window);
        return index >= 0 ? overview->items()[index].bounds : QRectF();
    }

    void show()
    {
        ASSERT_TRUE(overview->enter());
        settle();
        ASSERT_EQ(State::Shown, overview->state());
    }
};

} // namespace

TEST_F(OverviewTest, TheShortcutBringsItInAndTakesItAway)
{
    Window *first = makeWindow(QRect(100, 100, 500, 400));
    Window *second = makeWindow(QRect(300, 200, 500, 400));
    overview->toggle();
    EXPECT_EQ(State::Entering, overview->state());
    EXPECT_TRUE(node->isActive());
    settle();
    EXPECT_EQ(State::Shown, overview->state());

    // Both, apart, below the desk bar and inside the work area, smaller than
    // they are.
    ASSERT_EQ(2u, overview->items().size());
    const QRectF a = placeOf(first);
    const QRectF b = placeOf(second);
    EXPECT_FALSE(a.intersects(b));
    ASSERT_EQ(1u, overview->bars().size());
    for (const QRectF &place : {a, b}) {
        EXPECT_GE(place.top(), overview->bars()[0].bounds.bottom());
        EXPECT_TRUE(QRectF(kOutput).contains(place));
        EXPECT_LT(place.width(), 500);
    }
    // Where they were relative to each other.
    EXPECT_LT(a.left(), b.left());

    overview->toggle();
    EXPECT_EQ(State::Exiting, overview->state());
    settle();
    EXPECT_EQ(State::Hidden, overview->state());
    EXPECT_FALSE(node->isActive());
    EXPECT_TRUE(overview->items().empty());
    // Nothing changed hands.
    EXPECT_EQ(second, active());
}

TEST_F(OverviewTest, ThePointerAndTheKeysAreTheOverviewsWhileItIsUp)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    EXPECT_FALSE(overview->onPointerMotion(at(now), QPointF(300, 300)));
    EXPECT_FALSE(overview->onKey(at(now), KEY_A, true));
    EXPECT_FALSE(overview->onKey(at(now), KEY_A, false));

    show();
    // The window gets neither motion nor keys.
    EXPECT_TRUE(overview->onPointerMotion(at(now), placeOf(window).center()));
    EXPECT_EQ(0, overview->hovered());
    EXPECT_TRUE(overview->onKey(at(now), KEY_A, true));
    EXPECT_TRUE(overview->onKey(at(now), KEY_A, false));

    overview->exit();
    // On its way out the windows have their input again.
    EXPECT_FALSE(overview->onPointerMotion(at(now), QPointF(300, 300)));
    EXPECT_FALSE(overview->onKey(at(now), KEY_A, true));
    EXPECT_FALSE(overview->onKey(at(now), KEY_A, false));
    settle();
    EXPECT_EQ(State::Hidden, overview->state());
}

TEST_F(OverviewTest, ThePanelsAndTheDockKeepTheirInput)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    Window *dock = makeWindow(QRect(300, 740, 400, 60));
    dock->setRole(WindowRole::Dock);
    dock->setFocusPolicy(FocusPolicy::NoFocus);
    manager.activation()->activate(window);
    show();
    ASSERT_EQ(1u, overview->items().size());

    EXPECT_EQ(OverviewController::Hit::Type::Panel, overview->hitTest(QPointF(500, 770)).type);
    EXPECT_FALSE(overview->onPointerMotion(at(now), QPointF(500, 770)));
    EXPECT_FALSE(overview->onPointerButton(at(now), QPointF(500, 770), BTN_LEFT, true));
    EXPECT_FALSE(overview->onPointerButton(at(now), QPointF(500, 770), BTN_LEFT, false));
    EXPECT_EQ(State::Shown, overview->state());
    // Off the dock, the overview has it again.
    EXPECT_TRUE(overview->onPointerMotion(at(now), QPointF(500, 600)));
}

TEST_F(OverviewTest, AClickedWindowIsActivatedAndLandsOnTop)
{
    Window *first = makeWindow(QRect(100, 100, 500, 400));
    Window *second = makeWindow(QRect(300, 200, 500, 400));
    show();
    moveTo(placeOf(first).center());
    EXPECT_EQ(indexOf(first), overview->hovered());
    click(placeOf(first).center());
    EXPECT_EQ(State::Exiting, overview->state());
    EXPECT_EQ(first, active());
    // Drawn last on its way back.
    EXPECT_EQ(first, overview->items().back().window);
    settle();
    EXPECT_EQ(State::Hidden, overview->state());
    EXPECT_EQ(first, manager.windows().back());
    EXPECT_NE(second, active());
}

TEST_F(OverviewTest, EscapeAndAClickOnNothingLeaveAsItWas)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    show();
    key(KEY_ESC);
    EXPECT_EQ(State::Exiting, overview->state());
    settle();
    EXPECT_EQ(State::Hidden, overview->state());

    show();
    click(QPointF(10, 790));
    EXPECT_EQ(State::Exiting, overview->state());
    EXPECT_EQ(window, active());
}

TEST_F(OverviewTest, TheKeyboardPicksAWindow)
{
    Window *left = makeWindow(QRect(0, 200, 400, 300));
    Window *right = makeWindow(QRect(600, 200, 400, 300));
    show();
    // The active window first, then the one to its left.
    key(KEY_RIGHT);
    EXPECT_EQ(indexOf(right), overview->selected());
    key(KEY_LEFT);
    EXPECT_EQ(indexOf(left), overview->selected());
    key(KEY_ENTER);
    EXPECT_EQ(State::Exiting, overview->state());
    EXPECT_EQ(left, active());
}

TEST_F(OverviewTest, APressStaysWithItsWindowWhenTheOthersChange)
{
    Window *bottom = makeWindow(QRect(0, 100, 300, 300));
    Window *middle = makeWindow(QRect(350, 100, 300, 300));
    Window *top = makeWindow(QRect(700, 100, 300, 300));
    show();
    ASSERT_EQ(3u, overview->items().size());

    // The window under the press closes: letting go picks nothing.
    moveTo(placeOf(middle).center());
    input.onPointerButton(at(now), BTN_LEFT, true);
    manager.destroyWindow(middle);
    input.onPointerButton(at(now), BTN_LEFT, false);
    EXPECT_EQ(State::Shown, overview->state());
    EXPECT_EQ(top, active());

    // One below it closes: letting go still picks it.
    moveTo(placeOf(top).center());
    input.onPointerButton(at(now), BTN_LEFT, true);
    manager.destroyWindow(bottom);
    input.onPointerButton(at(now), BTN_LEFT, false);
    EXPECT_EQ(State::Exiting, overview->state());
    EXPECT_EQ(top, active());
}

TEST_F(OverviewTest, AKeyHeldSinceBeforeGoesUpAtTheWindow)
{
    makeWindow(QRect(100, 100, 500, 400));
    EXPECT_FALSE(overview->onKey(at(now), KEY_LEFTMETA, true));
    show();
    EXPECT_TRUE(overview->onKey(at(now), KEY_A, true));
    // The window saw it go down, so it sees it go up; the overview's own key
    // stays the overview's.
    EXPECT_FALSE(overview->onKey(at(now), KEY_LEFTMETA, false));
    EXPECT_TRUE(overview->onKey(at(now), KEY_A, false));
}

TEST_F(OverviewTest, ActivationFromElsewhereEndsIt)
{
    Window *first = makeWindow(QRect(100, 100, 500, 400));
    makeWindow(QRect(300, 200, 500, 400));
    show();
    // The dock asking for a window.
    manager.activation()->activate(first);
    EXPECT_EQ(State::Exiting, overview->state());
}

TEST_F(OverviewTest, MinimizedWindowsFadeInAndComeBackWhenPicked)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    window->minimize();
    ASSERT_TRUE(window->state()->isMinimized());
    show();
    ASSERT_EQ(1u, overview->items().size());
    EXPECT_TRUE(overview->items()[0].minimized);
    // Drawn from its live content, which is hidden with it.
    paint();
    EXPECT_TRUE(drewContentOf(window));
    click(placeOf(window).center());
    EXPECT_FALSE(window->state()->isMinimized());
    EXPECT_EQ(window, active());
}

TEST_F(OverviewTest, ClickingADeskShowsItsWindowsAndStaysUp)
{
    Window *first = makeWindow(QRect(100, 100, 500, 400));
    ASSERT_NE(nullptr, desks->newDesk(DesksCreationRemovalSource::Keyboard));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *second = makeWindow(QRect(200, 100, 500, 400));
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    ASSERT_EQ(first, active());
    show();
    ASSERT_EQ(1u, overview->items().size());
    click(overview->bars()[0].desks[1].bounds.center());
    EXPECT_EQ(desk(1), desks->activeDesk());
    EXPECT_EQ(State::Shown, overview->state());
    ASSERT_EQ(1u, overview->items().size());
    EXPECT_EQ(second, overview->items()[0].window);
    EXPECT_TRUE(overview->bars()[0].desks[1].active);
}

TEST_F(OverviewTest, ClickingTheDeskItIsOnTakesItAway)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    show();
    click(overview->bars()[0].desks[0].bounds.center());
    EXPECT_EQ(State::Exiting, overview->state());
    EXPECT_EQ(desk(0), desks->activeDesk());
    EXPECT_EQ(window, active());
}

TEST_F(OverviewTest, AWindowDraggedOntoADeskMovesThere)
{
    Window *moved = makeWindow(QRect(100, 100, 500, 400));
    Window *stays = makeWindow(QRect(300, 200, 500, 400));
    ASSERT_NE(nullptr, desks->newDesk(DesksCreationRemovalSource::Keyboard));
    show();
    const QPointF from = placeOf(moved).center();
    const QPointF to = overview->bars()[0].desks[1].bounds.center();
    moveTo(from);
    input.onPointerButton(at(now), BTN_LEFT, true);
    for (int step = 1; step <= 10; ++step)
        moveTo(from + (to - from) * step / 10);
    EXPECT_TRUE(overview->bars()[0].desks[1].highlighted);
    input.onPointerButton(at(now), BTN_LEFT, false);
    EXPECT_EQ(desk(1), desks->deskOf(moved));
    EXPECT_EQ(desk(0), desks->activeDesk());
    EXPECT_EQ(State::Shown, overview->state());
    ASSERT_EQ(1u, overview->items().size());
    EXPECT_EQ(stays, overview->items()[0].window);
}

TEST_F(OverviewTest, AWindowLetGoElsewhereGoesBack)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    show();
    const QRectF place = placeOf(window);
    moveTo(place.center());
    input.onPointerButton(at(now), BTN_LEFT, true);
    moveTo(place.center() + QPointF(60, 40));
    input.onPointerButton(at(now), BTN_LEFT, false);
    EXPECT_EQ(State::Shown, overview->state());
    EXPECT_EQ(place, placeOf(window));
    EXPECT_EQ(desk(0), desks->deskOf(window));
}

TEST_F(OverviewTest, ThreeFingersUpFollowAndDownLeave)
{
    // Large enough that the overview shows it smaller.
    Window *window = makeWindow(QRect(0, 30, 1000, 740));
    WmGestureHandler gestures(desks, {}, overview);
    input.setGestureFilter(&gestures);
    const auto swipe = [this](double mm) {
        const QPointF travel(0, mm / WmGestureHandler::kMmPerUnacceleratedUnit);
        input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel * 5, travel, 1, 0);
    };

    // Down on the desk does nothing.
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    swipe(10);
    EXPECT_EQ(State::Hidden, overview->state());
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);

    // A slowing hold, which the fingers show before they stop.
    const auto hold = [&](double direction) {
        for (int step = 0; step < 3; ++step) {
            now += 10;
            swipe(0.05 * direction);
        }
    };

    // Up, halfway and a bit, held, then let go: in.
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    for (int step = 0; step < 6; ++step) {
        now += 10;
        swipe(-WmGestureHandler::kOverviewSwipeLengthMm * 0.1);
    }
    hold(-1);
    EXPECT_EQ(State::Entering, overview->state());
    const double progress = node->progress(double(now) + 5);
    EXPECT_NEAR(0.6, progress, 0.05);
    const QRectF halfway = node->presentedRect(0, progress, double(now), nullptr);
    EXPECT_LT(halfway.width(), window->bounds().width());
    EXPECT_GT(halfway.width(), placeOf(window).width());
    now += 200;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    settle();
    EXPECT_EQ(State::Shown, overview->state());

    // Down a little, held, let go: it stays.
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    for (int step = 0; step < 3; ++step) {
        now += 10;
        swipe(WmGestureHandler::kOverviewSwipeLengthMm * 0.1);
    }
    hold(1);
    now += 200;
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    settle();
    EXPECT_EQ(State::Shown, overview->state());

    // A flick down: out.
    input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
    for (int step = 0; step < 3; ++step) {
        now += 8;
        swipe(WmGestureHandler::kOverviewSwipeLengthMm * 0.1);
    }
    input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    EXPECT_EQ(State::Exiting, overview->state());
    settle();
    EXPECT_EQ(State::Hidden, overview->state());
    input.setGestureFilter(nullptr);
}

TEST_F(OverviewTest, EveryDeskPreviewShowsItsWindows)
{
    makeWindow(QRect(100, 100, 500, 400));
    ASSERT_NE(nullptr, desks->newDesk(DesksCreationRemovalSource::Keyboard));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *there = makeWindow(QRect(200, 100, 500, 400));
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    ASSERT_FALSE(there->isVisible());
    show();
    bar->invalidatePreviews();
    // One preview a frame: the active desk's, then the other.
    paint();
    paint();
    EXPECT_TRUE(drewContentOf(there));
}

TEST_F(OverviewTest, ADraggedWindowShrinksOnItsWayToTheBar)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    ASSERT_NE(nullptr, desks->newDesk(DesksCreationRemovalSource::Keyboard));
    show();
    const QRectF place = placeOf(window);
    const int index = indexOf(window);
    const auto dragged = [&] {
        return node->presentedRect(index, 1, double(now), nullptr);
    };
    moveTo(place.center());
    input.onPointerButton(at(now), BTN_LEFT, true);
    // Away from the bar, the size it has in the overview.
    moveTo(place.center() + QPointF(0, -10));
    EXPECT_EQ(place.size(), dragged().size());
    EXPECT_TRUE(dragged().contains(place.center() + QPointF(0, -10)));

    // On the bar, the window at the expanded bar's share of the output's
    // height: 12 + 80 + 24 + 6 of 800.
    const auto bar = overview->bars()[0];
    const double expanded = 122;
    const QPointF onBar = bar.desks[1].bounds.center();
    moveTo(onBar);
    EXPECT_NEAR(500 * expanded / 800, dragged().width(), 0.01);
    EXPECT_NEAR(400 * expanded / 800, dragged().height(), 0.01);
    EXPECT_TRUE(dragged().contains(onBar));

    // Half its reach below the bar, halfway there.
    const QPointF between(onBar.x(), bar.bounds.top() + expanded + place.height() / 4);
    moveTo(between);
    EXPECT_NEAR((place.width() + 500 * expanded / 800) / 2, dragged().width(), 0.01);
    input.onPointerButton(at(now), BTN_LEFT, false);
}

TEST_F(OverviewTest, GlassIsDrawnWhereTheWindowIsShown)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    compositor::Surface *surface = window->rootSurface();
    surface->setBackgroundBlur(QRect(0, 0, 100, 400), 10);
    surface->commit();
    compositor.updateTextures(renderer);
    show();
    renderer.blurs.clear();
    paint();
    const QRectF place = placeOf(window);
    const double scale = place.width() / 500;
    ASSERT_EQ(1, renderer.blurs.size());
    EXPECT_EQ(QRectF(place.topLeft(), QSizeF(100, 400) * scale).toAlignedRect(),
              renderer.blurs[0]);
}

TEST_F(OverviewTest, APreviewsCloseButtonCombinesItsDeskIntoTheNext)
{
    Window *here = makeWindow(QRect(100, 100, 500, 400));
    ASSERT_NE(nullptr, desks->newDesk(DesksCreationRemovalSource::Keyboard));
    desks->activateDesk(desk(1), DesksSwitchSource::DeskSwitchShortcut);
    Window *there = makeWindow(QRect(200, 100, 500, 400));
    desks->activateDesk(desk(0), DesksSwitchSource::DeskSwitchShortcut);
    show();
    ASSERT_EQ(1u, overview->items().size());

    // Only on the preview the pointer is on.
    const QRectF preview = overview->bars()[0].desks[1].bounds;
    moveTo(preview.center());
    EXPECT_FALSE(overview->bars()[0].desks[0].closeShown);
    EXPECT_TRUE(overview->bars()[0].desks[1].closeShown);
    EXPECT_FALSE(overview->bars()[0].desks[1].closeHovered);
    const QRectF button = scene::OverviewDeskBarNode::closeButtonRect(preview);
    EXPECT_TRUE(preview.contains(button));
    moveTo(button.center());
    EXPECT_TRUE(overview->bars()[0].desks[1].closeHovered);

    click(button.center());
    ASSERT_EQ(1u, desks->desks().size());
    EXPECT_EQ(desk(0), desks->deskOf(there));
    EXPECT_EQ(State::Shown, overview->state());
    EXPECT_EQ(2u, overview->items().size());
    EXPECT_GE(indexOf(here), 0);
    // One desk left, which cannot go.
    moveTo(overview->bars()[0].desks[0].bounds.center());
    EXPECT_FALSE(overview->bars()[0].desks[0].closeShown);
}

TEST_F(OverviewTest, EvenOneDeskGetsAPreview)
{
    makeWindow(QRect(100, 100, 500, 400));
    show();
    ASSERT_EQ(1u, overview->bars().size());
    const auto one = overview->bars()[0];
    ASSERT_EQ(1u, one.desks.size());
    EXPECT_TRUE(one.desks[0].active);
    // A preview at the output's shape, the new-desk button half as wide.
    const QRectF preview = one.desks[0].bounds;
    EXPECT_NEAR(double(kOutput.width()) / kOutput.height(), preview.width() / preview.height(),
                0.02);
    EXPECT_EQ(preview.height(), one.newDesk.height());
    EXPECT_NEAR(preview.width() / 2, one.newDesk.width(), 1);
    EXPECT_EQ(QStringLiteral("New"), one.newDeskLabel);
    const QRectF place = overview->items()[0].bounds;

    click(one.newDesk.center());
    EXPECT_EQ(2u, desks->desks().size());
    // Not switched to, and the windows stay where they are.
    EXPECT_EQ(desk(0), desks->activeDesk());
    EXPECT_EQ(State::Shown, overview->state());
    EXPECT_EQ(2u, overview->bars()[0].desks.size());
    EXPECT_EQ(one.bounds.height(), overview->bars()[0].bounds.height());
    now += 1500;
    EXPECT_EQ(place, overview->items()[0].bounds);
}

TEST_F(OverviewTest, AWindowDroppedOnNewGoesToANewDesk)
{
    Window *window = makeWindow(QRect(100, 100, 500, 400));
    makeWindow(QRect(300, 200, 500, 400));
    show();
    const QPointF from = placeOf(window).center();
    const QPointF to = overview->bars()[0].newDesk.center();
    moveTo(from);
    input.onPointerButton(at(now), BTN_LEFT, true);
    for (int step = 1; step <= 10; ++step)
        moveTo(from + (to - from) * step / 10);
    EXPECT_TRUE(overview->bars()[0].newDeskHighlighted);
    input.onPointerButton(at(now), BTN_LEFT, false);
    ASSERT_EQ(2u, desks->desks().size());
    EXPECT_EQ(desk(1), desks->deskOf(window));
    EXPECT_EQ(desk(0), desks->activeDesk());
}

TEST_F(OverviewTest, TheCursorIsTheDefaultOneThroughout)
{
    makeWindow(QRect(100, 100, 500, 400));
    ASSERT_FALSE(seat.cursorOverride().has_value());
    ASSERT_TRUE(overview->enter());
    EXPECT_EQ(input::CursorShape::Default, seat.cursorOverride());
    settle();
    key(KEY_ESC);
    // Heading out, the windows have the pointer again, but not the cursor.
    EXPECT_EQ(State::Exiting, overview->state());
    EXPECT_EQ(input::CursorShape::Default, seat.cursorOverride());
    settle();
    EXPECT_EQ(State::Hidden, overview->state());
    EXPECT_FALSE(seat.cursorOverride().has_value());
}

TEST_F(OverviewTest, ThreeFingersDownOverAWindowBringItToTheFront)
{
    Window *below = makeWindow(QRect(100, 100, 500, 400));
    Window *above = makeWindow(QRect(300, 200, 500, 400));
    WmGestureHandler gestures(desks, {}, overview);
    input.setGestureFilter(&gestures);
    const auto swipe = [this](double mm) {
        const QPointF travel(0, mm / WmGestureHandler::kMmPerUnacceleratedUnit);
        input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel * 5, travel, 1, 0);
    };
    const auto flickDown = [&] {
        input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
        for (int step = 0; step < 3; ++step) {
            now += 8;
            swipe(WmGestureHandler::kOverviewSwipeLengthMm * 0.1);
        }
        input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
        settle();
    };

    // Away from every window: the desk as it was.
    show();
    moveTo(QPointF(990, 790));
    flickDown();
    EXPECT_EQ(State::Hidden, overview->state());
    EXPECT_EQ(above, active());

    // Over the one below: it comes to the front.
    show();
    moveTo(placeOf(below).center());
    ASSERT_EQ(indexOf(below), overview->hovered());
    flickDown();
    EXPECT_EQ(State::Hidden, overview->state());
    EXPECT_EQ(below, active());
    input.setGestureFilter(nullptr);
}

TEST_F(OverviewTest, AClickWhileItComesInTakesTheWindowUnderThePointer)
{
    Window *first = makeWindow(QRect(100, 100, 400, 300));
    Window *second = makeWindow(QRect(500, 300, 400, 300));
    ASSERT_EQ(second, active());
    ASSERT_TRUE(overview->enter());
    paint();
    now += 60;
    const double progress = node->progress(double(now));
    ASSERT_GT(progress, 0.1);
    ASSERT_LT(progress, 0.9);
    ASSERT_EQ(State::Entering, overview->state());
    // Where it is drawn now, not where it is going.
    const QRectF drawn = node->presentedRect(indexOf(first), progress, double(now), nullptr);
    click(drawn.center());
    EXPECT_EQ(State::Exiting, overview->state());
    EXPECT_EQ(first, active());
}

TEST_F(OverviewTest, TheShortcutTurnsItAroundWhereverItIs)
{
    makeWindow(QRect(100, 100, 500, 400));
    ASSERT_TRUE(overview->enter());
    paint();
    for (const State heading : {State::Exiting, State::Entering, State::Exiting}) {
        now += 50;
        const double before = node->progress(double(now));
        overview->toggle();
        EXPECT_EQ(heading, overview->state());
        EXPECT_NEAR(before, node->progress(double(now)), 1e-9);
        paint();
    }
    overview->toggle();
    settle();
    EXPECT_EQ(State::Shown, overview->state());
}

TEST_F(OverviewTest, ASidewaysSwipeLandsItWhereItWasHeading)
{
    makeWindow(QRect(100, 100, 500, 400));
    WmGestureHandler gestures(desks, {}, overview);
    input.setGestureFilter(&gestures);
    const auto sideways = [this] {
        const QPointF travel(10 / WmGestureHandler::kMmPerUnacceleratedUnit, 0);
        input.onPointerGestureBegin(at(now), input::GestureType::Swipe, 3);
        input.onPointerGestureUpdate(at(now), input::GestureType::Swipe, travel * 5, travel, 1,
                                     0);
        input.onPointerGestureEnd(at(now), input::GestureType::Swipe, false);
    };

    ASSERT_TRUE(overview->enter());
    paint();
    now += 60;
    ASSERT_EQ(State::Entering, overview->state());
    sideways();
    EXPECT_EQ(State::Shown, overview->state());
    EXPECT_EQ(1, node->progress(double(now)));

    key(KEY_ESC);
    paint();
    now += 60;
    ASSERT_EQ(State::Exiting, overview->state());
    sideways();
    EXPECT_EQ(State::Hidden, overview->state());
    input.setGestureFilter(nullptr);
}
