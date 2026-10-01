// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The server-side frame, from the window manager's side of it.
//
// The protocol half lives in xdg_decoration_protocol_test.cpp. What is
// asserted here is what the frame does to geometry, where its hit regions are, what a press on
// one of them asks the window manager to do, and that none of it reaches the
// client's surface.

#include <gtest/gtest.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "compositor/compositor.h"
#include "compositor/shm_pool.h"
#include "compositor/surface.h"
#include "decoration/decoration.h"
#include "decoration/decoration_theme.h"
#include "decoration/shadow_node.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/pointer.h"
#include "input/pointer_delegate.h"
#include "input/seat.h"
#include "input/touch.h"
#include "input/touch_delegate.h"
#include "render/rounded_shape.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "scene/surface_node.h"
#include "scene/window_animation.h"
#include "shm_test_util.h"
#include "wm/activation.h"
#include "wm/resize_handles.h"
#include "wm/shadow_controller.h"
#include "wm/shell_surface.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_visual_controller.h"
#include "wm/window_state.h"
#include "wm/wm_shadow_controller_delegate.h"

using namespace cutefish;
using namespace cutefish::wm;

namespace {

constexpr QRect kOutput(0, 0, 1000, 800);

TEST(DecorationThemeTest, DarkModeSwapsThePaletteAndTellsObservers)
{
    struct Observer : decoration::ThemeObserver {
        int changes = 0;
        void onThemeChanged() override { ++changes; }
    } observer;
    decoration::Theme theme;
    theme.addObserver(&observer);
    const float lightTitleBar = theme.palette().titleBar.r;

    theme.setDarkMode(true);
    EXPECT_EQ(observer.changes, 1);
    EXPECT_LT(theme.palette().titleBar.r, lightTitleBar);
    EXPECT_GT(theme.captionColor(true).r, theme.palette().titleBar.r);

    theme.setDarkMode(true);
    EXPECT_EQ(observer.changes, 1);

    theme.removeObserver(&observer);
    theme.setDarkMode(false);
    EXPECT_EQ(observer.changes, 1);
    EXPECT_FLOAT_EQ(theme.palette().titleBar.r, lightTitleBar);
}

TEST(DecorationRasterTest, CaptionHoverRemainsCircular)
{
    const auto highlight = decoration::renderButtonHighlight(24, {1, 1, 1, 1});
    ASSERT_EQ(highlight.size(), QSize(20, 20));
    const auto alpha = [&](int x, int y) {
        return highlight.bits()[(y * highlight.size().width() + x) * 4 + 3];
    };
    for (const QPoint point : {QPoint(2, 2), {17, 2}, {2, 17}, {17, 17}})
        EXPECT_LT(alpha(point.x(), point.y()), 32);
    EXPECT_EQ(alpha(10, 10), 255);
    EXPECT_GT(alpha(10, 0), 240);

    decoration::Image window({20, 20});
    decoration::fillContinuousRoundedRect(window, {0, 0, 20, 20}, 10, {1, 1, 1, 1});
    EXPECT_GT(window.bits()[(2 * 20 + 2) * 4 + 3], 240);
}

input::Timestamp at(int ms)
{
    return input::Timestamp(std::chrono::milliseconds(ms));
}

// Only what these tests need from a client pointer: whether the client was told
// anything at all.
class RecordingPointerDelegate : public input::PointerDelegate
{
public:
    std::vector<compositor::Surface *> enters;
    std::vector<compositor::Surface *> leaves;
    int motions = 0;
    int buttons = 0;

    void onPointerDestroying(input::Pointer *) override { }

    bool canAcceptPointerEventsForSurface(compositor::Surface *) const override { return true; }

    void onPointerEnter(compositor::Surface *surface, const QPointF &, input::ButtonFlags) override
    {
        enters.push_back(surface);
    }

    void onPointerLeave(compositor::Surface *surface) override { leaves.push_back(surface); }

    void onPointerMotion(input::Timestamp, const QPointF &) override { ++motions; }

    void onPointerButton(input::Timestamp, input::ButtonFlags, bool) override { ++buttons; }

    void onPointerScroll(input::Timestamp, const QPointF &, bool) override { }

    void onFingerScrollStop(input::Timestamp) override { }

    void onPointerFrame() override { }
};

class RecordingTouchDelegate : public input::TouchDelegate
{
public:
    std::vector<std::string> calls;

    void onTouchDestroying(input::Touch *) override { }

    bool canAcceptTouchEventsForSurface(compositor::Surface *) const override { return true; }

    void onTouchDown(compositor::Surface *, input::Timestamp, int32_t, const QPointF &) override
    {
        calls.push_back("down");
    }

    void onTouchUp(input::Timestamp, int32_t) override { calls.push_back("up"); }

    void onTouchMotion(input::Timestamp, int32_t, const QPointF &) override
    {
        calls.push_back("motion");
    }

    void onTouchFrame() override { }

    void onTouchCancel() override { calls.push_back("cancel"); }
};

struct Recorder {
    std::vector<ShellSurface::ConfigureState> configures;
    std::vector<uint32_t> serials;
    int closes = 0;
    uint32_t nextSerial = 1;

    ShellSurface::ConfigureState last() const { return configures.back(); }
};

class DecorationTest : public testing::Test
{
protected:
    scene::Scene scene;
    scene::Node *surfaceContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    scene::Node *cursorContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("cursor")));
    compositor::Compositor compositor {surfaceContainer};
    testsupport::FakeRenderer renderer;
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    WindowManager manager {&compositor, &seat};
    decoration::Theme theme;
    decoration::DecorationProvider provider {&theme};
    // Shadows are no longer the frame's; the controller has to be here for the
    // scene the frame appears in to be the real one.
    decoration::ShadowProvider shadowProvider {&theme};
    WmShadowControllerDelegate shadowDelegate;
    ShadowController shadowController {&manager, &shadowProvider, &shadowDelegate};

    std::vector<std::unique_ptr<compositor::Surface>> surfaces;
    std::vector<std::unique_ptr<ShellSurface>> shells;
    std::vector<std::unique_ptr<Recorder>> recorders;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(8 << 20);

    void SetUp() override
    {
        scene.setSize(kOutput.size());
        seat.setCursorContainer(cursorContainer);
        manager.setOutputBounds(kOutput);
        manager.setFrameProvider(&provider);
        input.setBounds(kOutput.size());
        input.setEventSink(&manager);
    }

    void TearDown() override
    {
        shells.clear();
        seat.shutdown();
    }

    Insets insets() const { return theme.metrics().insets(); }

    compositor::Surface *makeSurface()
    {
        surfaces.push_back(compositor.createSurface());
        return surfaces.back().get();
    }

    void attach(compositor::Surface *surface, const QSize &size)
    {
        auto buffer = pool->createBuffer(size, 0, size.width() * 4, render::PixelFormat::Bgrx8888,
                                         nullptr);
        surface->attach(buffer, {});
        surface->damage(QRect(QPoint(), size));
    }

    ShellSurface *makeShell(Recorder **recorderOut = nullptr)
    {
        recorders.push_back(std::make_unique<Recorder>());
        Recorder *recorder = recorders.back().get();
        shells.push_back(std::make_unique<ShellSurface>(&manager, makeSurface()));
        ShellSurface *shell = shells.back().get();

        shell->setConfigureCallback([recorder](const ShellSurface::ConfigureState &state) {
            recorder->configures.push_back(state);
            recorder->serials.push_back(recorder->nextSerial);
            return recorder->nextSerial++;
        });
        shell->setCloseCallback([recorder] {
            ++recorder->closes;
        });

        if (recorderOut)
            *recorderOut = recorder;
        return shell;
    }

    // The whole xdg-shell handshake, with the decoration negotiated before the
    // first buffer, as the protocol requires.
    ShellSurface *makeToplevel(const QSize &size, bool serverSide = true,
                               Recorder **recorderOut = nullptr)
    {
        Recorder *recorder = nullptr;
        ShellSurface *shell = makeShell(&recorder);
        shell->setToplevel();
        shell->setHasDecorationObject(true);
        shell->setRequestedDecorationMode(serverSide ? ShellSurface::DecorationMode::ServerSide
                                                     : ShellSurface::DecorationMode::ClientSide);
        shell->rootSurface()->commit();
        EXPECT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
        attach(shell->rootSurface(), size);
        shell->rootSurface()->commit();
        compositor.updateTextures(renderer);
        if (recorderOut)
            *recorderOut = recorder;
        return shell;
    }

    // A toplevel that never negotiates decoration at all.
    ShellSurface *makeUndecoratedToplevel(const QSize &size, Recorder **recorderOut = nullptr)
    {
        Recorder *recorder = nullptr;
        ShellSurface *shell = makeShell(&recorder);
        shell->setToplevel();
        shell->rootSurface()->commit();
        EXPECT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
        attach(shell->rootSurface(), size);
        shell->rootSurface()->commit();
        compositor.updateTextures(renderer);
        if (recorderOut)
            *recorderOut = recorder;
        return shell;
    }

    decoration::Decoration *frameOf(Window *window)
    {
        return static_cast<decoration::Decoration *>(window->frame());
    }

    // Output coordinates of a point in the window's frame.
    QPointF onFrame(Window *window, const QPoint &framePoint)
    {
        return QPointF(window->bounds().topLeft() + framePoint);
    }

    QPointF onTitleBar(Window *window)
    {
        return onFrame(window,
                       QPoint(window->bounds().width() / 2, theme.metrics().titleBarHeight / 2));
    }

    void press(const QPointF &location)
    {
        input.warpPointer(location);
        input.onPointerButton(at(1), BTN_LEFT, true);
    }

    void release(const QPointF &location)
    {
        input.warpPointer(location);
        input.onPointerButton(at(2), BTN_LEFT, false);
    }

    // The touchscreen spans the output: libinput's coordinates are normalized.
    static QPointF normalized(const QPointF &location)
    {
        return QPointF(location.x() / kOutput.width(), location.y() / kOutput.height());
    }

    void touchDown(int ms, int32_t id, const QPointF &location)
    {
        input.onTouchDown(at(ms), id, normalized(location));
        input.onTouchFrame(at(ms));
    }

    void touchMotion(int ms, int32_t id, const QPointF &location)
    {
        input.onTouchMotion(at(ms), id, normalized(location));
        input.onTouchFrame(at(ms));
    }

    void touchUp(int ms, int32_t id)
    {
        input.onTouchUp(at(ms), id);
        input.onTouchFrame(at(ms));
    }
};

////////////////////////////////////////////////////////////////////////////////
// Geometry

TEST_F(DecorationTest, TheFrameSitsOutsideTheClientArea)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    ASSERT_NE(nullptr, window);
    ASSERT_NE(nullptr, window->frame());

    // The outer bounds are the client area plus the frame; the client's own
    // geometry is untouched by it.
    EXPECT_EQ(insets(), window->frameInsets());
    EXPECT_EQ(QSize(400, 300), window->contentBounds().size());
    EXPECT_EQ(insets().outset(QSize(400, 300)), window->bounds().size());
    EXPECT_EQ(window->bounds().topLeft() + insets().topLeft(), window->contentBounds().topLeft());
    // And the client's buffer starts below the titlebar, not under it.
    EXPECT_EQ(window->contentBounds().topLeft(), shell->rootSurface()->node()->absolutePosition());
}

TEST_F(DecorationTest, TheConfigureSizeDescribesTheClientAreaAndNotTheFrame)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeToplevel(QSize(400, 300), true, &recorder);
    EXPECT_EQ(QSize(400, 300), shell->window()->contentBounds().size());

    shell->window()->requestBounds(QRect(100, 100, 500, 400));
    EXPECT_EQ(insets().inset(QSize(500, 400)), recorder->last().size);
}

TEST_F(DecorationTest, AClientDecoratedWindowKeepsItsOwnGeometry)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300), /*serverSide=*/false);
    Window *window = shell->window();

    EXPECT_EQ(nullptr, window->frame());
    EXPECT_TRUE(window->frameInsets().isNull());
    EXPECT_EQ(QSize(400, 300), window->bounds().size());
    EXPECT_EQ(window->bounds(), window->contentBounds());
}

TEST_F(DecorationTest, AClientThatNeverNegotiatesIsNotDecorated)
{
    ShellSurface *shell = makeUndecoratedToplevel(QSize(400, 300));
    EXPECT_EQ(ShellSurface::DecorationMode::ClientSide, shell->decorationMode());
    EXPECT_EQ(nullptr, shell->window()->frame());
    EXPECT_EQ(QSize(400, 300), shell->window()->bounds().size());
}

TEST_F(DecorationTest, TurningTheFrameOnKeepsTheClientAreaWhereItIs)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300), /*serverSide=*/false);
    Window *window = shell->window();
    const QRect content = window->contentBounds();

    shell->setRequestedDecorationMode(ShellSurface::DecorationMode::ServerSide);

    ASSERT_NE(nullptr, window->frame());
    EXPECT_EQ(content, window->contentBounds());
    EXPECT_EQ(insets().outset(content), window->bounds());
}

TEST_F(DecorationTest, TurningTheFrameOffGivesTheOuterBoundsBackToTheClient)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect content = window->contentBounds();

    shell->setRequestedDecorationMode(ShellSurface::DecorationMode::ClientSide);

    EXPECT_EQ(nullptr, window->frame());
    EXPECT_EQ(content, window->bounds());
    EXPECT_EQ(content, window->contentBounds());
}

TEST_F(DecorationTest, AMaximizedWindowFillsTheWorkAreaAndTheClientGetsWhatIsLeft)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeToplevel(QSize(400, 300), true, &recorder);
    Window *window = shell->window();

    window->maximize();

    EXPECT_EQ(manager.workAreaFor(window), window->bounds());
    const Insets maximizedInsets {0, insets().top, 0, 0};
    EXPECT_EQ(maximizedInsets.inset(manager.workAreaFor(window)), window->contentBounds());
    EXPECT_EQ(maximizedInsets.inset(kOutput.size()), recorder->last().size);
    EXPECT_EQ(maximizedInsets, window->frameInsets());
}

TEST_F(DecorationTest, RestoringComesBackToTheOuterBoundsItLeft)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect bounds = window->bounds();

    window->maximize();
    window->restore();

    EXPECT_EQ(bounds, window->bounds());
    EXPECT_EQ(insets().inset(bounds), window->contentBounds());
}

TEST_F(DecorationTest, FullscreenTakesTheFrameAwayAndLeavingItBringsItBack)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeToplevel(QSize(400, 300), true, &recorder);
    Window *window = shell->window();
    const QRect bounds = window->bounds();

    window->setFullscreen(true);

    EXPECT_EQ(kOutput, window->bounds());
    EXPECT_TRUE(window->frameInsets().isNull());
    EXPECT_EQ(kOutput, window->contentBounds());
    EXPECT_EQ(kOutput.size(), recorder->last().size);
    // The frame is still there, it just occupies nothing.
    EXPECT_NE(nullptr, window->frame());

    window->setFullscreen(false);

    EXPECT_EQ(bounds, window->bounds());
    EXPECT_EQ(insets(), window->frameInsets());
    EXPECT_EQ(insets().inset(bounds), window->contentBounds());
}

TEST_F(DecorationTest, TheMinimumSizeCoversTheFrameAsWellAsTheClientArea)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    shell->setMinimumSize(QSize(200, 150));
    shell->rootSurface()->commit();

    Window *window = shell->window();
    EXPECT_EQ(QSize(200, 150), window->minimumSize());
    EXPECT_EQ(insets().outset(QSize(200, 150)), window->minimumOuterSize());
}

////////////////////////////////////////////////////////////////////////////////
// Hit testing

TEST_F(DecorationTest, TheFrameOwnsItsTitleBarButNotTheClientArea)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();

    FrameArea area = FrameArea::None;
    EXPECT_EQ(window, manager.frameAt(onTitleBar(window), &area));
    EXPECT_EQ(FrameArea::TitleBar, area);

    EXPECT_EQ(nullptr, manager.frameAt(QPointF(window->contentBounds().center())));
}

TEST_F(DecorationTest, EveryEdgeAndCornerResolvesToItsResizeEdge)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    decoration::Decoration *frame = frameOf(window);
    const QRect bounds = window->bounds();

    struct Case {
        QPoint point;
        ResizeEdge edge;
    };

    const int right = bounds.width() - 1;
    const int bottom = bounds.height() - 1;
    const Case cases[] = {
            {QPoint(bounds.width() / 2, 0), ResizeEdge::Top},
            {QPoint(bounds.width() / 2, bottom), ResizeEdge::Bottom},
            {QPoint(0, bounds.height() / 2), ResizeEdge::Left},
            {QPoint(right, bounds.height() / 2), ResizeEdge::Right},
            {QPoint(0, 0), ResizeEdge::TopLeft},
            {QPoint(right, 0), ResizeEdge::TopRight},
            {QPoint(0, bottom), ResizeEdge::BottomLeft},
            {QPoint(right, bottom), ResizeEdge::BottomRight},
    };

    for (const Case &testCase : cases) {
        EXPECT_EQ(FrameArea::ResizeBorder, frame->areaAt(onFrame(window, testCase.point)))
                << toString(testCase.edge);

        press(onFrame(window, testCase.point));
        EXPECT_TRUE(manager.eventHandler()->isDragging()) << toString(testCase.edge);
        EXPECT_EQ(testCase.edge, manager.eventHandler()->draggedEdge()) << toString(testCase.edge);
        manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
    }
}

TEST_F(DecorationTest, TheResizeGripReachesOutsideTheWindow)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QPoint outside = window->bounds().bottomRight() + QPoint(2, 2);

    EXPECT_EQ(FrameArea::ResizeBorder, frameOf(window)->areaAt(QPointF(outside)));
    EXPECT_EQ(window, manager.frameAt(QPointF(outside)));
}

// The window ends at its rounded outline, not at the square frame: the border
// follows the arc inwards, and the corner reaches along each edge past it.
TEST_F(DecorationTest, TheResizeBorderFollowsTheRoundedCorner)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    decoration::Decoration *frame = frameOf(window);
    const decoration::Metrics metrics;
    const auto visual = wm::WindowVisualController::evaluate(*window);
    ASSERT_GT(visual.shape.radius, metrics.resizeMargin);
    const render::RoundedShape shape {QRectF(QPoint(), window->bounds().size()),
                                      visual.shape.radius};

    // Down the top-left diagonal: resize wherever the pixel is within the margin
    // of the arc, the title bar past that.
    bool beyondTheSquareBorder = false;
    for (int i = 0; i < 20; ++i) {
        const QPoint point(i, i);
        const bool border =
                shape.distance(QPointF(point) + QPointF(0.5, 0.5)) > -metrics.resizeMargin;
        beyondTheSquareBorder |= border && i >= metrics.resizeMargin;
        EXPECT_EQ(border ? FrameArea::ResizeBorder : FrameArea::TitleBar,
                  frame->areaAt(onFrame(window, point)))
                << i;
        if (!border)
            continue;
        press(onFrame(window, point));
        EXPECT_EQ(ResizeEdge::TopLeft, manager.eventHandler()->draggedEdge()) << i;
        manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
    }
    EXPECT_TRUE(beyondTheSquareBorder);

    const int corner = std::max(
            metrics.resizeCorner,
            int(std::ceil(render::cornerExtent(shape.radius, shape.bounds.width() / 2)))
                    + metrics.resizeMargin);
    ASSERT_GT(corner, metrics.resizeCorner);
    press(onFrame(window, QPoint(corner - 1, 0)));
    EXPECT_EQ(ResizeEdge::TopLeft, manager.eventHandler()->draggedEdge());
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
    press(onFrame(window, QPoint(corner, 0)));
    EXPECT_EQ(ResizeEdge::Top, manager.eventHandler()->draggedEdge());
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
}

TEST_F(DecorationTest, ACaptionButtonIsNotTheTitleBar)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    decoration::Decoration *frame = frameOf(window);

    ASSERT_EQ(3u, frame->buttons().size());
    for (const decoration::CaptionButton &button : frame->buttons()) {
        EXPECT_EQ(FrameArea::CaptionButton,
                  frame->areaAt(onFrame(window, button.geometry().center())));
    }
}

TEST_F(DecorationTest, TheButtonsCentreInTheTitleBar)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    decoration::Decoration *frame = frameOf(shell->window());
    const auto &metrics = theme.metrics();
    const int width = shell->window()->bounds().width();

    const auto &buttons = frame->buttons();
    ASSERT_EQ(3u, buttons.size());
    const int top = (metrics.titleBarHeight - metrics.buttonSize) / 2;
    EXPECT_EQ(width - metrics.buttonsRightMargin, buttons.back().geometry().right() + 1);
    for (size_t index = 0; index < buttons.size(); ++index) {
        const QRect geometry = buttons[index].geometry();
        EXPECT_EQ(QSize(metrics.buttonSize, metrics.buttonSize), geometry.size());
        EXPECT_EQ(top, geometry.top());
        if (index > 0)
            EXPECT_EQ(metrics.buttonSpacing,
                      geometry.left() - buttons[index - 1].geometry().right() - 1);
    }
}

TEST_F(DecorationTest, TheDebugBoundsAreTheFrameInOutputCoordinates)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    decoration::Decoration *frame = frameOf(window);
    const QPoint origin = window->bounds().topLeft();

    EXPECT_EQ(QRect(origin, QSize(window->bounds().width(), theme.metrics().titleBarHeight)),
              frame->titleBarBounds());
    const std::vector<QRect> bounds = frame->captionButtonBounds();
    ASSERT_EQ(frame->buttons().size(), bounds.size());
    for (size_t index = 0; index < bounds.size(); ++index)
        EXPECT_EQ(frame->buttons()[index].geometry().translated(origin), bounds[index]);
}

TEST_F(DecorationTest, AWindowUnderAnotherOneDoesNotGetTheFrameHit)
{
    ShellSurface *lower = makeToplevel(QSize(400, 300));
    lower->window()->setBounds(QRect(100, 100, 402, 332));
    ShellSurface *upper = makeToplevel(QSize(400, 300));
    upper->window()->setBounds(QRect(100, 100, 402, 332));
    manager.raise(upper->window());

    EXPECT_EQ(upper->window(), manager.frameAt(onTitleBar(upper->window())));
}

////////////////////////////////////////////////////////////////////////////////
// Input

TEST_F(DecorationTest, PressingTheTitleBarStartsAMove)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect bounds = window->bounds();

    press(onTitleBar(window));
    ASSERT_TRUE(manager.eventHandler()->isDragging());
    EXPECT_EQ(window, manager.eventHandler()->draggedWindow());
    EXPECT_EQ(ResizeEdge::None, manager.eventHandler()->draggedEdge());

    input.onPointerMotion(at(2), QPointF(40, 30));
    EXPECT_EQ(bounds.translated(40, 30), window->bounds());

    release(onTitleBar(window));
    EXPECT_FALSE(manager.eventHandler()->isDragging());
}

TEST_F(DecorationTest, TheClientNeverSeesInputOverTheFrame)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();

    RecordingPointerDelegate delegate;
    input::Pointer pointer(&delegate, &seat);

    // Start inside the client area, so the client has the pointer.
    input.warpPointer(QPointF(window->contentBounds().center()));
    input.onPointerMotion(at(1), QPointF(0, 0));
    ASSERT_EQ(1u, delegate.enters.size());
    EXPECT_EQ(shell->rootSurface(), delegate.enters.front());
    const int motions = delegate.motions;

    // Then onto the titlebar: a leave, and nothing after it.
    input.warpPointer(onTitleBar(window));
    input.onPointerMotion(at(2), QPointF(0, 0));
    EXPECT_EQ(1u, delegate.leaves.size());
    EXPECT_EQ(motions, delegate.motions);

    input.onPointerButton(at(3), BTN_LEFT, true);
    EXPECT_EQ(0, delegate.buttons);
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
}

TEST_F(DecorationTest, ADragFromTheClientAreaKeepsThePointerOverTheFrame)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();

    RecordingPointerDelegate delegate;
    input::Pointer pointer(&delegate, &seat);

    input.warpPointer(QPointF(window->contentBounds().center()));
    input.onPointerMotion(at(1), QPointF(0, 0));
    input.onPointerButton(at(2), BTN_LEFT, true);
    ASSERT_EQ(1, delegate.buttons);
    const int motions = delegate.motions;

    // The drag wanders over the titlebar and then off the window entirely: the
    // frame must not take it, and the client must keep receiving motion.
    input.warpPointer(onTitleBar(window));
    input.onPointerMotion(at(3), QPointF(0, 0));
    input.warpPointer(QPointF(900, 700));
    input.onPointerMotion(at(4), QPointF(0, 0));
    EXPECT_TRUE(delegate.leaves.empty());
    EXPECT_EQ(motions + 2, delegate.motions);
    EXPECT_FALSE(manager.eventHandler()->isDragging());

    input.onPointerButton(at(5), BTN_LEFT, false);
    EXPECT_EQ(2, delegate.buttons);
}

TEST_F(DecorationTest, TheCloseButtonAsksTheClientToClose)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeToplevel(QSize(400, 300), true, &recorder);
    Window *window = shell->window();
    const QRect close = frameOf(window)->buttons().back().geometry();

    press(onFrame(window, close.center()));
    EXPECT_TRUE(frameOf(window)->isTrackingPress());
    EXPECT_EQ(0, recorder->closes);

    release(onFrame(window, close.center()));
    EXPECT_EQ(1, recorder->closes);
    // Asking is all it does: the window is still here until the client acts.
    EXPECT_FALSE(manager.windows().empty());
}

TEST_F(DecorationTest, APressThatWandersOffItsButtonDoesNothing)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeToplevel(QSize(400, 300), true, &recorder);
    Window *window = shell->window();
    const QRect close = frameOf(window)->buttons().back().geometry();

    press(onFrame(window, close.center()));
    release(onTitleBar(window));

    EXPECT_EQ(0, recorder->closes);
    EXPECT_FALSE(frameOf(window)->isTrackingPress());
    // And no move was started by the release either.
    EXPECT_FALSE(manager.eventHandler()->isDragging());
}

TEST_F(DecorationTest, TheMinimizeButtonMinimizes)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect minimize = frameOf(window)->buttons().front().geometry();

    press(onFrame(window, minimize.center()));
    release(onFrame(window, minimize.center()));

    EXPECT_EQ(WindowStateType::Minimized, window->stateType());
    EXPECT_FALSE(window->isVisible());
}

TEST_F(DecorationTest, TheMaximizeButtonMaximizesAndThenRestores)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeToplevel(QSize(400, 300), true, &recorder);
    Window *window = shell->window();
    const QRect maximize = frameOf(window)->buttons()[1].geometry();

    press(onFrame(window, maximize.center()));
    release(onFrame(window, maximize.center()));
    EXPECT_EQ(WindowStateType::Maximized, window->stateType());
    EXPECT_TRUE(frameOf(window)->isRestoreButton());

    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    attach(shell->rootSurface(), window->contentBounds().size());
    shell->rootSurface()->commit();
    // The button follows the newly presented frame.
    const QRect restore = frameOf(window)->buttons()[1].geometry();
    press(onFrame(window, restore.center()));
    release(onFrame(window, restore.center()));
    EXPECT_EQ(WindowStateType::Normal, window->stateType());
}

TEST_F(DecorationTest, DoubleClickingTheTitleBarTogglesMaximize)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();

    const QPointF point = onTitleBar(window);
    input.warpPointer(point);
    input.onPointerButton(at(0), BTN_LEFT, true);
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
    input.onPointerButton(at(0), BTN_LEFT, false);
    input.onPointerButton(at(100), BTN_LEFT, true);

    EXPECT_EQ(WindowStateType::Maximized, window->stateType());
}

TEST_F(DecorationTest, AFingerOnTheTitleBarDragsTheWindow)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect bounds = window->bounds();
    const QPointF start = onTitleBar(window);

    touchDown(1, 1, start);
    ASSERT_TRUE(manager.eventHandler()->isTouchDrag());
    EXPECT_EQ(window, manager.eventHandler()->draggedWindow());

    touchMotion(2, 1, start + QPointF(40, 30));
    EXPECT_EQ(bounds.translated(40, 30), window->bounds());

    // Neither the pointer nor another finger moves or ends it.
    input.onPointerMotion(at(3), QPointF(100, 100));
    input.onPointerButton(at(3), BTN_LEFT, true);
    input.onPointerButton(at(3), BTN_LEFT, false);
    touchDown(3, 2, QPointF(900, 700));
    touchMotion(3, 2, QPointF(800, 600));
    touchUp(3, 2);
    EXPECT_TRUE(manager.eventHandler()->isDragging());
    EXPECT_EQ(bounds.translated(40, 30), window->bounds());

    touchUp(4, 1);
    EXPECT_FALSE(manager.eventHandler()->isDragging());
    EXPECT_EQ(bounds.translated(40, 30), window->bounds());
}

TEST_F(DecorationTest, AFingerOnAResizeBorderResizes)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect bounds = window->bounds();
    const QPointF left = onFrame(window, {-2, 100});

    touchDown(1, 1, left);
    ASSERT_TRUE(manager.eventHandler()->isTouchDrag());
    EXPECT_EQ(ResizeEdge::Left, manager.eventHandler()->draggedEdge());
    touchMotion(2, 1, left - QPointF(30, 0));
    EXPECT_EQ(bounds.adjusted(-30, 0, 0, 0), window->bounds());
    touchUp(3, 1);
    EXPECT_FALSE(manager.eventHandler()->isDragging());
}

////////////////////////////////////////////////////////////////////////////////
// Server resize handles on client-decorated windows

class ServerResizeTest : public DecorationTest
{
protected:
    Window *makeWindow(bool serverResize = true)
    {
        ShellSurface *shell = makeToplevel(QSize(400, 300), /*serverSide=*/false);
        shell->rootSurface()->setServerResize(serverResize);
        Window *window = shell->window();
        window->setBounds(QRect(100, 100, 400, 300));
        return window;
    }

    ResizeEdge edgeAt(const QPointF &location, bool touch = false)
    {
        ResizeEdge edge = ResizeEdge::None;
        manager.resizeHandleAt(location, touch, &edge);
        return edge;
    }
};

TEST_F(ServerResizeTest, OnlyAWindowThatAsksGetsHandles)
{
    Window *window = makeWindow(/*serverResize=*/false);
    const QPointF right(window->bounds().right() + 3, window->bounds().center().y());

    press(right);
    EXPECT_FALSE(manager.eventHandler()->isDragging());
    release(right);

    window->rootSurface()->setServerResize(true);
    press(right);
    ASSERT_TRUE(manager.eventHandler()->isDragging());
    EXPECT_EQ(window, manager.eventHandler()->draggedWindow());
    EXPECT_EQ(ResizeEdge::Right, manager.eventHandler()->draggedEdge());
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
}

TEST_F(ServerResizeTest, TheHandlesReachOutsideAndOnePixelIn)
{
    Window *window = makeWindow();
    const QRect bounds = window->bounds();
    const int y = bounds.center().y();

    EXPECT_EQ(ResizeEdge::Right, edgeAt(QPointF(bounds.right() + kResizeOutsideBoundsSize, y)));
    EXPECT_EQ(ResizeEdge::None, edgeAt(QPointF(bounds.right() + kResizeOutsideBoundsSize + 1, y)));
    EXPECT_EQ(ResizeEdge::Right, edgeAt(QPointF(bounds.right(), y)));
    EXPECT_EQ(ResizeEdge::None, edgeAt(QPointF(bounds.right() - 1, y)));
    EXPECT_EQ(ResizeEdge::Left, edgeAt(QPointF(bounds.left() - 1, y)));
    EXPECT_EQ(ResizeEdge::Top, edgeAt(QPointF(bounds.center().x(), bounds.top() - 1)));
    EXPECT_EQ(ResizeEdge::Bottom, edgeAt(QPointF(bounds.center().x(), bounds.bottom() + 1)));
    // A finger gets five times the reach outside.
    EXPECT_EQ(ResizeEdge::Left, edgeAt(QPointF(bounds.left() - 20, y), /*touch=*/true));
}

TEST_F(ServerResizeTest, TheCornersFollowTheRoundedOutline)
{
    Window *window = makeWindow();
    const QRect bounds = window->bounds();

    // Inside the square but outside the arc.
    EXPECT_EQ(ResizeEdge::TopLeft, edgeAt(QPointF(bounds.topLeft() + QPoint(1, 1))));
    EXPECT_EQ(ResizeEdge::TopLeft, edgeAt(QPointF(bounds.topLeft() - QPoint(3, 3))));
    EXPECT_EQ(ResizeEdge::BottomRight, edgeAt(QPointF(bounds.bottomRight() + QPoint(3, 3))));
    EXPECT_EQ(ResizeEdge::TopRight,
              edgeAt(QPointF(bounds.right() - kResizeAreaCornerSize + 1, bounds.top() - 1)));
    EXPECT_EQ(ResizeEdge::None, edgeAt(QPointF(bounds.center())));
}

TEST_F(ServerResizeTest, TheClientLosesThePointerOverAHandle)
{
    Window *window = makeWindow();

    RecordingPointerDelegate delegate;
    input::Pointer pointer(&delegate, &seat);

    input.warpPointer(QPointF(window->bounds().center()));
    input.onPointerMotion(at(1), QPointF(0, 0));
    ASSERT_EQ(1u, delegate.enters.size());

    input.warpPointer(QPointF(window->bounds().right(), window->bounds().center().y()));
    input.onPointerMotion(at(2), QPointF(0, 0));
    EXPECT_EQ(1u, delegate.leaves.size());

    input.onPointerButton(at(3), BTN_LEFT, true);
    EXPECT_EQ(0, delegate.buttons);
    EXPECT_EQ(ResizeEdge::Right, manager.eventHandler()->draggedEdge());
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
}

TEST_F(ServerResizeTest, AFingerOnAHandleResizes)
{
    Window *window = makeWindow();
    const QRect bounds = window->bounds();
    const QPointF left(bounds.left() - 20, bounds.center().y());

    touchDown(1, 1, left);
    ASSERT_TRUE(manager.eventHandler()->isTouchDrag());
    EXPECT_EQ(ResizeEdge::Left, manager.eventHandler()->draggedEdge());
    touchMotion(2, 1, left - QPointF(30, 0));
    EXPECT_EQ(bounds.adjusted(-30, 0, 0, 0), window->bounds());
    touchUp(3, 1);
    EXPECT_FALSE(manager.eventHandler()->isDragging());
}

TEST_F(ServerResizeTest, AWindowAboveCoversTheHandles)
{
    Window *lower = makeWindow();
    ShellSurface *upper = makeToplevel(QSize(100, 100), /*serverSide=*/false);
    upper->window()->setBounds(QRect(lower->bounds().right() - 50, 300, 100, 100));
    manager.raise(upper->window());

    EXPECT_EQ(ResizeEdge::None, edgeAt(QPointF(lower->bounds().right() + 3, 350)));
    EXPECT_EQ(ResizeEdge::Right, edgeAt(QPointF(lower->bounds().right() + 3, 150)));
}

TEST_F(ServerResizeTest, AMaximizedWindowHasNoHandles)
{
    Window *window = makeWindow();
    window->maximize();

    EXPECT_EQ(ResizeEdge::None, edgeAt(QPointF(window->bounds().topLeft())));
    EXPECT_EQ(ResizeEdge::None, edgeAt(QPointF(window->bounds().center().x(), 0)));
}

TEST_F(DecorationTest, TappingTheCloseButtonAsksTheClientToClose)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeToplevel(QSize(400, 300), true, &recorder);
    Window *window = shell->window();
    const QRect close = frameOf(window)->buttons().back().geometry();

    // A tap that slides off the button does nothing.
    touchDown(1, 1, onFrame(window, close.center()));
    touchMotion(2, 1, onTitleBar(window));
    touchUp(3, 1);
    EXPECT_EQ(0, recorder->closes);
    EXPECT_FALSE(manager.eventHandler()->isDragging());

    touchDown(4, 1, onFrame(window, close.center()));
    // The pointer does not take a button a finger holds.
    EXPECT_FALSE(frameOf(window)->isTrackingPress());
    EXPECT_EQ(0, recorder->closes);
    touchUp(5, 1);
    EXPECT_EQ(1, recorder->closes);
}

TEST_F(DecorationTest, DoubleTappingTheTitleBarTogglesMaximize)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QPointF point = onTitleBar(window);

    touchDown(0, 1, point);
    touchUp(50, 1);
    // A finger lands a little off where it did the first time.
    touchDown(150, 1, point + QPointF(12, 5));

    EXPECT_EQ(WindowStateType::Maximized, window->stateType());
    EXPECT_FALSE(manager.eventHandler()->isDragging());
    touchUp(200, 1);
}

TEST_F(DecorationTest, AClientMovesItsWindowWithTheFingerItWasTouched)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300), false);
    Window *window = shell->window();
    const QRect bounds = window->bounds();
    RecordingTouchDelegate delegate;
    input::Touch touch(&delegate, &seat);

    // Without a finger on the window there is nothing to move it with.
    EXPECT_FALSE(shell->startMove(true));

    const QPointF start = QPointF(window->contentBounds().topLeft()) + QPointF(100, 10);
    touchDown(1, 1, start);
    touchMotion(2, 1, start + QPointF(2, 0));
    ASSERT_EQ((std::vector<std::string>{"down", "motion"}), delegate.calls);

    // As a client does from its own titlebar, with the touch-down's serial.
    ASSERT_TRUE(shell->startMove(true));
    EXPECT_TRUE(manager.eventHandler()->isTouchDrag());
    EXPECT_EQ("cancel", delegate.calls.back());

    touchMotion(3, 1, start + QPointF(52, 40));
    EXPECT_EQ(bounds.translated(50, 40), window->bounds());
    touchUp(4, 1);
    EXPECT_FALSE(manager.eventHandler()->isDragging());
    // The rest of the point went to the drag, not the client.
    EXPECT_EQ((std::vector<std::string>{"down", "motion", "cancel"}), delegate.calls);
}

TEST_F(DecorationTest, TheDeviceCancellingItsPointsEndsATouchDrag)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect bounds = window->bounds();

    touchDown(1, 1, onTitleBar(window));
    touchMotion(2, 1, onTitleBar(window) + QPointF(20, 20));
    input.onTouchCancel(at(3));
    EXPECT_FALSE(manager.eventHandler()->isDragging());
    // It ends where it was, as ash completes a gesture drag.
    EXPECT_EQ(bounds.translated(20, 20), window->bounds());
}

TEST_F(DecorationTest, PressingTheFrameActivatesTheWindow)
{
    ShellSurface *lower = makeToplevel(QSize(400, 300));
    ShellSurface *upper = makeToplevel(QSize(400, 300));
    upper->window()->setBounds(QRect(500, 400, 402, 332));
    ASSERT_TRUE(upper->window()->isActive());

    press(onTitleBar(lower->window()));
    EXPECT_TRUE(lower->window()->isActive());
    EXPECT_FALSE(upper->window()->isActive());
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
}

TEST_F(DecorationTest, APopupIsPlacedAgainstTheClientAreaAndIsNeverDecorated)
{
    ShellSurface *parent = makeToplevel(QSize(400, 300));
    Window *window = parent->window();

    ShellSurface *popup = makeShell();
    ASSERT_TRUE(popup->setPopup(parent, QRect(10, 20, 120, 90)));
    attach(popup->rootSurface(), QSize(120, 90));
    popup->rootSurface()->commit();

    // The popup's coordinates are relative to the parent's xdg window geometry,
    // which starts inside the frame.
    EXPECT_EQ(window->contentBounds().topLeft() + QPoint(10, 20), popup->absoluteOrigin());
    EXPECT_EQ(popup->absoluteOrigin(), popup->rootSurface()->node()->absolutePosition());
    ASSERT_NE(nullptr, popup->window());
    EXPECT_TRUE(popup->window()->isPopup());
    EXPECT_FALSE(popup->window()->isDecorated());
    EXPECT_EQ(nullptr, popup->window()->frame());
    EXPECT_EQ(window, popup->ownerWindow());
    // And the frame does not take input meant for it.
    EXPECT_EQ(nullptr, manager.frameAt(QPointF(popup->absoluteOrigin() + QPoint(60, 45))));
}

TEST_F(DecorationTest, ATransientToplevelKeepsItsParentAndItsFrame)
{
    ShellSurface *parent = makeToplevel(QSize(400, 300));
    ShellSurface *child = makeShell();
    child->setToplevel();
    child->setHasDecorationObject(true);
    ASSERT_TRUE(child->setTransientParent(parent));
    child->rootSurface()->commit();
    EXPECT_TRUE(child->acknowledgeConfigure(1));
    attach(child->rootSurface(), QSize(200, 150));
    child->rootSurface()->commit();

    ASSERT_NE(nullptr, child->window());
    EXPECT_NE(nullptr, child->window()->frame());
    EXPECT_EQ(parent->window(), child->window()->transientParent());
    EXPECT_EQ(QSize(200, 150), child->window()->contentBounds().size());
}

////////////////////////////////////////////////////////////////////////////////
// Activation

TEST_F(DecorationTest, TheFrameFollowsActivation)
{
    ShellSurface *first = makeToplevel(QSize(400, 300));
    EXPECT_TRUE(frameOf(first->window())->isActive());

    ShellSurface *second = makeToplevel(QSize(400, 300));
    EXPECT_FALSE(frameOf(first->window())->isActive());
    EXPECT_TRUE(frameOf(second->window())->isActive());

    manager.activation()->activate(first->window());
    EXPECT_TRUE(frameOf(first->window())->isActive());
    EXPECT_FALSE(frameOf(second->window())->isActive());
}

////////////////////////////////////////////////////////////////////////////////
// Scene

TEST_F(DecorationTest, TheFrameIsSceneContentOfItsOwn)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();

    scene::Node *windowNode = window->node();
    ASSERT_NE(nullptr, windowNode);
    EXPECT_EQ(window->bounds(), windowNode->geometry());
    // The shadow stays outside the visual body shared by frame and client.
    ASSERT_EQ(2u, windowNode->children().size());
    EXPECT_EQ(QStringLiteral("shadow"), windowNode->children()[0]->name());
    EXPECT_EQ(QStringLiteral("decoration"), window->visualNode()->children()[0]->name());
    EXPECT_EQ(shell->rootSurface()->node(), window->visualNode()->children().back());

    // And it draws something.
    renderer.calls.clear();
    scene.render(renderer);
    EXPECT_FALSE(renderer.calls.isEmpty());
}

TEST_F(DecorationTest, CaptionButtonIconsAreCentered)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    decoration::Decoration *frame = frameOf(window);

    renderer.calls.clear();
    scene.render(renderer);

    std::vector<QRect> iconTargets;
    const QSize iconSize(theme.metrics().iconSize, theme.metrics().iconSize);
    for (const auto &call : renderer.calls) {
        if (call.kind == QStringLiteral("texture") && call.texture
            && call.texture->size() == iconSize) {
            iconTargets.push_back(call.rect);
        }
    }

    ASSERT_EQ(frame->buttons().size(), iconTargets.size());
    for (size_t i = 0; i < iconTargets.size(); ++i) {
        const QRect button =
                frame->buttons()[i].geometry().translated(window->bounds().topLeft());
        const QRect icon = iconTargets[i];
        EXPECT_EQ(2 * button.x() + button.width(), 2 * icon.x() + icon.width());
        EXPECT_EQ(2 * button.y() + button.height(), 2 * icon.y() + icon.height());
    }
}

TEST_F(DecorationTest, AMinimizedWindowHidesItsFrameWithIt)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    scene::Node *decorationNode = window->visualNode()->children()[0];

    ASSERT_TRUE(decorationNode->isEffectivelyVisible());
    window->minimize();
    EXPECT_FALSE(decorationNode->isEffectivelyVisible());
    window->restore();
    EXPECT_TRUE(decorationNode->isEffectivelyVisible());
}

TEST_F(DecorationTest, AFullscreenWindowDrawsNoFrame)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    scene::Node *decorationNode = window->visualNode()->children()[0];

    window->setFullscreen(true);
    EXPECT_FALSE(decorationNode->isEffectivelyVisible());
    EXPECT_EQ(FrameArea::None, frameOf(window)->areaAt(QPointF(window->bounds().center())));

    window->setFullscreen(false);
    EXPECT_TRUE(decorationNode->isEffectivelyVisible());
}

////////////////////////////////////////////////////////////////////////////////
// Lifetime

TEST_F(DecorationTest, TheFrameGoesWithTheWindow)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    scene::Node *windowNode = window->node();
    compositor::Surface *surface = shell->rootSurface();

    // Unmapping destroys the window; the client's surface node survives it.
    surface->attach(nullptr, {});
    surface->commit();

    EXPECT_EQ(nullptr, shell->window());
    EXPECT_TRUE(manager.windows().empty());
    EXPECT_NE(nullptr, surface->node());
    EXPECT_NE(windowNode, surface->node()->parent());
    EXPECT_EQ(surfaceContainer, surface->node()->parent());
}

TEST_F(DecorationTest, ADecorationTornDownFromInsideItsOwnCallbackIsSurvivable)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();
    const QRect close = frameOf(window)->buttons().back().geometry();

    // A client that reacts to xdg_toplevel.close by unmapping straight away,
    // which destroys the window and the frame from inside the button release.
    shell->setCloseCallback([this, shell] {
        shell->rootSurface()->attach(nullptr, {});
        shell->rootSurface()->commit();
        // And maps something else while the old frame is being torn down.
        makeToplevel(QSize(200, 100));
    });

    press(onFrame(window, close.center()));
    release(onFrame(window, close.center()));

    EXPECT_EQ(nullptr, shell->window());
    EXPECT_EQ(1u, manager.windows().size());
}

TEST_F(DecorationTest, DestroyingTheShellSurfaceUnderAHoveredFrameIsSurvivable)
{
    ShellSurface *shell = makeToplevel(QSize(400, 300));
    Window *window = shell->window();

    input.warpPointer(onTitleBar(window));
    input.onPointerMotion(at(1), QPointF(0, 0));

    auto it = std::find_if(shells.begin(), shells.end(),
                           [shell](const std::unique_ptr<ShellSurface> &owned) {
                               return owned.get() == shell;
                           });
    shells.erase(it);

    EXPECT_TRUE(manager.windows().empty());
    // The next motion has nothing to leave and must not reach a dead frame.
    input.onPointerMotion(at(2), QPointF(5, 5));
}

} // namespace

TEST_F(DecorationTest, ShadowPolicyGeometryStackingAndLifetime)
{
    auto *shell = makeToplevel({500, 350});
    auto *window = shell->window();
    window->setBounds(QRect(QPoint(100, 150), insets().outset(QSize(500, 350))));
    window->activate();
    const QRect content = window->contentBounds();
    const QRect inputBounds = frameOf(window)->inputBounds();
    const auto paint = [&] {
        renderer.calls.clear();
        scene.render(renderer);
    };
    const auto exteriorCount = [&] {
        int count = 0;
        for (const auto &call : renderer.calls) {
            if (call.kind == "texture" && !window->bounds().contains(call.rect))
                ++count;
        }
        return count;
    };
    paint();
    EXPECT_EQ(exteriorCount(), 8);
    const auto misses = renderer.textureCacheMisses();
    EXPECT_EQ(window->contentBounds(), content);
    EXPECT_EQ(frameOf(window)->inputBounds(), inputBounds);
    EXPECT_EQ(frameOf(window)->areaAt(window->bounds().topLeft() - QPoint(20, 20)),
              FrameArea::None);
    window->setBounds(window->bounds().translated(30, 20));
    paint();
    EXPECT_EQ(renderer.textureCacheMisses(), misses);
    window->maximize();
    paint();
    EXPECT_EQ(exteriorCount(), 0);
    window->restore();
    paint();
    EXPECT_EQ(exteriorCount(), 8);
    window->setFullscreen(true);
    paint();
    EXPECT_EQ(exteriorCount(), 0);
    window->setFullscreen(false);
    window->minimize();
    paint();
    EXPECT_EQ(exteriorCount(), 0);
    window->restore();
    paint();
    EXPECT_EQ(exteriorCount(), 8);

    const QRect beforeSwitch = window->contentBounds();
    for (int i = 0; i < 3; ++i) {
        window->setDecorated(false);
        paint();
        // The shadow is the window's, not the frame's: taking the frame away
        // leaves it drawn.
        EXPECT_EQ(exteriorCount(), 8);
        EXPECT_EQ(window->contentBounds(), beforeSwitch);
        window->setDecorated(true);
        paint();
        EXPECT_EQ(exteriorCount(), 8);
        EXPECT_EQ(window->contentBounds(), beforeSwitch);
    }
    auto *transient = makeToplevel({300, 200})->window();
    ASSERT_TRUE(transient->setTransientParent(window));
    transient->activate();
    paint();
    EXPECT_FALSE(window->isActive());
    EXPECT_TRUE(transient->isActive());
    const auto &siblings = window->node()->parent()->children();
    EXPECT_LT(std::find(siblings.begin(), siblings.end(), window->node()),
              std::find(siblings.begin(), siblings.end(), transient->node()));
    // Each window draws its shadow under its frame, and its frame under its
    // client, within the same group.
    EXPECT_EQ(window->node()->children().front()->name(), "shadow");
    EXPECT_EQ(window->visualNode()->children()[0]->name(), "decoration");
    EXPECT_EQ(transient->node()->children().front()->name(), "shadow");
    EXPECT_EQ(transient->visualNode()->children()[0]->name(), "decoration");
    EXPECT_GE(renderer.textureCacheMisses(), misses + 1);
    window->setCloseCallback([&] {
        shells.clear();
    });
    window->requestClose();
    EXPECT_TRUE(manager.windows().empty());
    paint();
    for (const auto &call : renderer.calls)
        EXPECT_NE(call.kind, "texture");
}

TEST_F(DecorationTest, ShadowCallbacksCanRetireOtherDecoratedWindows)
{
    struct RetireOther : WindowObserver {
        WindowManager *manager = nullptr;
        Window *victim = nullptr;
        bool onActivation = false;
        bool onState = false;
        int retirements = 0;

        void retire()
        {
            auto *removing = victim;
            victim = nullptr;
            if (removing) {
                ++retirements;
                manager->destroyWindow(removing);
            }
        }

        void onWindowActivationChanged(Window *, bool active) override
        {
            if (onActivation && active)
                retire();
        }

        void onWindowStateTypeChanged(Window *, WindowStateType) override
        {
            if (onState)
                retire();
        }

        void onWindowDestroying(Window *) override { retire(); }
    } observer;

    observer.manager = &manager;
    auto *window = makeToplevel({400, 300})->window();
    window->addObserver(&observer);
    observer.victim = makeToplevel({250, 180})->window();
    scene.render(renderer);
    observer.onActivation = true;
    window->activate();
    EXPECT_EQ(observer.retirements, 1);
    observer.onActivation = false;
    observer.victim = makeToplevel({260, 190})->window();
    observer.onState = true;
    window->maximize();
    EXPECT_EQ(observer.retirements, 2);
    window->restore();
    observer.victim = makeToplevel({270, 200})->window();
    scene.render(renderer);
    manager.destroyWindow(window);
    EXPECT_EQ(observer.retirements, 3);
    EXPECT_TRUE(manager.windows().empty());
    renderer.calls.clear();
    scene.render(renderer);
    for (const auto &call : renderer.calls)
        EXPECT_NE(call.kind, "texture");
}

TEST_F(DecorationTest, ShadowCoverageDoesNotStealUnderlyingInput)
{
    auto *lower = makeToplevel({600, 440})->window();
    lower->setBounds(QRect(QPoint(80, 80), insets().outset(QSize(600, 440))));
    auto *upper = makeToplevel({300, 200})->window();
    upper->setBounds(QRect(QPoint(240, 180), insets().outset(QSize(300, 200))));
    upper->activate();
    scene.render(renderer);
    // Below the top window's resize border, inside its painted shadow.
    const QPoint below(upper->bounds().center().x(), upper->bounds().bottom() + 11);
    EXPECT_EQ(manager.windowAt(below), lower);
    EXPECT_EQ(manager.frameAt(below), nullptr);
    // Moving shadow coverage over the lower titlebar cannot intercept its frame.
    upper->setBounds(QRect(QPoint(240, -145), upper->bounds().size()));
    scene.render(renderer);
    EXPECT_EQ(manager.frameAt({390, 100}), lower);
}

TEST_F(DecorationTest, ResizeCursorTracksEdgesAndDragOverrideEndsOnCancel)
{
    auto *shell = makeToplevel(QSize(400, 300));
    auto *window = shell->window();
    const auto left = onFrame(window, {-2, 100});
    input.onPointerMotionAbsolute(at(1), left);
    EXPECT_EQ(seat.compositorCursorShape(), input::CursorShape::EwResize);
    input.onPointerMotionAbsolute(at(2), onFrame(window, {-2, -2}));
    EXPECT_EQ(seat.compositorCursorShape(), input::CursorShape::NwseResize);
    ASSERT_TRUE(manager.eventHandler()->beginResize(window, ResizeEdge::TopRight, left));
    EXPECT_EQ(seat.cursorOverride(), input::CursorShape::NeswResize);
    input.onPointerMotionAbsolute(at(3), {600, 500});
    EXPECT_EQ(seat.cursorOverride(), input::CursorShape::NeswResize);
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Revert);
    EXPECT_FALSE(seat.cursorOverride());
    ASSERT_TRUE(manager.eventHandler()->beginMove(window, onTitleBar(window)));
    EXPECT_EQ(seat.cursorOverride(), input::CursorShape::Grabbing);
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Success);
    EXPECT_FALSE(seat.cursorOverride());
}

TEST_F(DecorationTest, UnicodeCaptionFitsAndRefreshesAfterTitleAndWidthChanges)
{
    auto *shell = makeToplevel({500, 300});
    auto *window = shell->window();
    shell->setTitle(QStringLiteral("文件管理器 — العربية — a long document title for elision"));
    auto paintCaption = [&] {
        renderer.calls.clear();
        scene.render(renderer);
        const QRect area = static_cast<decoration::Decoration *>(frameOf(window))
                                   ->captionArea()
                                   .translated(window->bounds().topLeft());
        bool found = false;
        for (const auto &call : renderer.calls) {
            if (call.kind == "texture" && call.rect.width() > 40
                && call.rect.height() < theme.metrics().titleBarHeight
                && call.rect.top() >= area.top() && call.rect.top() < area.bottom()) {
                EXPECT_TRUE(area.contains(call.rect));
                found = true;
            }
        }
        EXPECT_TRUE(found);
    };
    paintCaption();
    const auto textures = renderer.createdTextures;
    paintCaption();
    EXPECT_EQ(renderer.createdTextures, textures);
    shell->setTitle(QStringLiteral("另一个标题 — another document"));
    paintCaption();
    EXPECT_GT(renderer.createdTextures, textures);
    window->setBounds(QRect(window->bounds().topLeft(), QSize(280, 330)));
    paintCaption();
}

TEST_F(DecorationTest, CaptionTexelsMatchPhysicalPixelsAndEachScaleKeepsItsOwn)
{
    auto *shell = makeToplevel({500, 300});
    auto *window = shell->window();
    shell->setTitle(QStringLiteral("Fractional caption"));
    const auto caption = [&] {
        renderer.calls.clear();
        scene.render(renderer);
        const QRect area = static_cast<decoration::Decoration *>(frameOf(window))
                                   ->captionArea()
                                   .translated(window->bounds().topLeft());
        for (const auto &call : renderer.calls) {
            if (call.kind == "texture" && call.rect.width() > 40
                && call.rect.top() >= area.top() && call.rect.top() < area.bottom())
                return call;
        }
        ADD_FAILURE() << "no caption";
        return testsupport::FakeRenderer::Call {};
    };
    int textures = 0;
    for (const double scale : {1.25, 2.0, 1.25, 2.0}) {
        renderer.setRasterScale(scale);
        const auto call = caption();
        ASSERT_NE(call.texture, nullptr);
        // Rasterized at the scale it is shown at: one texel per physical pixel.
        EXPECT_EQ(call.texture->size(),
                  QSize(qRound(call.rect.width() * scale), qRound(call.rect.height() * scale)))
                << scale;
        EXPECT_TRUE(call.mapping.isIdentity());
        if (!textures)
            textures = renderer.createdTextures;
    }
    // One raster per scale, not one per frame as the outputs alternate.
    EXPECT_EQ(renderer.createdTextures, textures + 1);
}

TEST_F(DecorationTest, CaptionChangesDamageTheFrame)
{
    auto *shell = makeToplevel({500, 300});
    auto *window = shell->window();
    static_cast<scene::WindowAnimation *>(window->node())->finish();
    scene::DamageTracker tracker;
    tracker.update(*scene.root(), kOutput);
    ASSERT_TRUE(tracker.update(*scene.root(), kOutput).isEmpty());
    shell->setTitle(QStringLiteral("Updated caption"));
    EXPECT_FALSE(tracker.update(*scene.root(), kOutput).isEmpty());
}
