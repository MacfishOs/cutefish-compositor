// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/shell_surface_unittest.cc
//           chromium/src/components/exo/xdg_shell_surface_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: exercises the corresponding behavior with native
// compositor test doubles and APIs.
//
// Adapted: no views::Widget and no Ash, so the assertions are about the
// Cutefish Window and about what the configure callback was handed. The cases
// that only exercise ChromeOS state (snap, pip, float, desks) are dropped.

#include <gtest/gtest.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "compositor/compositor.h"
#include "compositor/shm_pool.h"
#include "compositor/surface.h"
#include "decoration/decoration_theme.h"
#include "decoration/shadow_node.h"
#include "display/display_manager.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "scene/surface_node.h"
#include "scene/window_animation.h"
#include "shm_test_util.h"
#include "wm/activation.h"
#include "wm/shadow_controller.h"
#include "wm/shell_surface.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"
#include "wm/wm_shadow_controller_delegate.h"

using namespace cutefish;
using namespace cutefish::wm;

namespace {

constexpr QRect kOutput(0, 0, 1000, 800);

input::Timestamp at(int ms)
{
    return input::Timestamp(std::chrono::milliseconds(ms));
}

// Everything a client would have seen.
struct Recorder {
    std::vector<ShellSurface::ConfigureState> configures;
    std::vector<uint32_t> serials;
    std::vector<QRect> popupConfigures;
    int closes = 0;
    uint32_t nextSerial = 1;

    ShellSurface::ConfigureState last() const { return configures.back(); }
};

class ShellSurfaceTest : public testing::Test
{
protected:
    decoration::Theme shadowTheme;
    decoration::ShadowProvider shadowProvider {&shadowTheme};
    WmShadowControllerDelegate shadowDelegate;
    scene::Scene scene;
    scene::Node *surfaceContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    compositor::Compositor compositor {surfaceContainer};
    testsupport::FakeRenderer renderer;
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    WindowManager manager {&compositor, &seat};

    std::vector<std::unique_ptr<compositor::Surface>> surfaces;
    std::vector<std::unique_ptr<ShellSurface>> shells;
    std::vector<std::unique_ptr<Recorder>> recorders;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(8 << 20);

    void SetUp() override
    {
        scene.setSize(kOutput.size());
        manager.setOutputBounds(kOutput);
        input.setBounds(kOutput.size());
        input.setEventSink(&manager);
    }

    void TearDown() override
    {
        shells.clear();
        seat.shutdown();
    }

    // Removes a shell surface from the fixture without destroying it, so the
    // destruction can run outside the container it was in.
    std::unique_ptr<ShellSurface> takeShell(ShellSurface *shell)
    {
        auto it = std::find_if(shells.begin(), shells.end(),
                               [shell](const std::unique_ptr<ShellSurface> &owned) {
                                   return owned.get() == shell;
                               });
        std::unique_ptr<ShellSurface> owned = std::move(*it);
        shells.erase(it);
        return owned;
    }

    // The same for a surface.
    std::unique_ptr<compositor::Surface> takeSurface(compositor::Surface *surface)
    {
        auto it = std::find_if(surfaces.begin(), surfaces.end(),
                               [surface](const std::unique_ptr<compositor::Surface> &owned) {
                                   return owned.get() == surface;
                               });
        std::unique_ptr<compositor::Surface> owned = std::move(*it);
        surfaces.erase(it);
        return owned;
    }

    // Where a client's buffer actually lands on screen. A toplevel's surface
    // node hangs under its window's node, so its own position is relative to
    // the window; a popup's is not.
    static QPoint surfacePosition(ShellSurface *shell)
    {
        return shell->rootSurface()->node()->absolutePosition();
    }

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

    // An xdg_surface with the recorder wired to its configure callbacks.
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
        shell->setPopupConfigureCallback([recorder](const QRect &bounds) {
            recorder->popupConfigures.push_back(bounds);
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

    // Drives the whole xdg-shell handshake: role, initial configure, ack, first
    // buffer.
    ShellSurface *makeMappedToplevel(const QSize &size, Recorder **recorderOut = nullptr)
    {
        Recorder *recorder = nullptr;
        ShellSurface *shell = makeShell(&recorder);
        shell->setToplevel();
        shell->rootSurface()->commit();
        EXPECT_EQ(1u, recorder->configures.size());
        EXPECT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
        attach(shell->rootSurface(), size);
        shell->rootSurface()->commit();
        compositor.updateTextures(renderer);
        if (recorderOut)
            *recorderOut = recorder;
        return shell;
    }
};

////////////////////////////////////////////////////////////////////////////////
// Initial configure and mapping

TEST_F(ShellSurfaceTest, TheFirstCommitWithoutABufferAsksForTheInitialConfigure)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);
    shell->setToplevel();

    EXPECT_TRUE(recorder->configures.empty());
    shell->rootSurface()->commit();

    ASSERT_EQ(1u, recorder->configures.size());
    // Nothing is mapped yet, and the compositor has no opinion on the size.
    EXPECT_EQ(nullptr, shell->window());
    EXPECT_EQ(QSize(0, 0), recorder->last().size);
    EXPECT_EQ(WindowStateType::Normal, recorder->last().stateType);
    EXPECT_FALSE(recorder->last().activated);
}

TEST_F(ShellSurfaceTest, NoRoleMeansNoConfigure)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);

    shell->rootSurface()->commit();

    EXPECT_TRUE(recorder->configures.empty());
    EXPECT_EQ(nullptr, shell->window());
}

TEST_F(ShellSurfaceTest, TheInitialConfigureCarriesARequestedMaximizedState)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);
    shell->setToplevel();
    shell->maximize();

    shell->rootSurface()->commit();

    ASSERT_EQ(1u, recorder->configures.size());
    EXPECT_EQ(WindowStateType::Maximized, recorder->last().stateType);
    EXPECT_EQ(kOutput.size(), recorder->last().size);
}

TEST_F(ShellSurfaceTest, TheInitialConfigureCarriesARequestedFullscreenState)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);
    shell->setToplevel();
    shell->setFullscreen(true);

    shell->rootSurface()->commit();

    ASSERT_EQ(1u, recorder->configures.size());
    EXPECT_EQ(WindowStateType::Fullscreen, recorder->last().stateType);
    EXPECT_EQ(kOutput.size(), recorder->last().size);
}

TEST_F(ShellSurfaceTest, ABufferAfterTheAckMapsAWindow)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);

    Window *window = shell->window();
    ASSERT_NE(nullptr, window);
    EXPECT_EQ(QSize(300, 200), window->bounds().size());
    EXPECT_TRUE(window->isVisible());
    EXPECT_TRUE(window->isActive());
    // Activation is a state change, so the client is told about it.
    EXPECT_TRUE(recorder->last().activated);
}

TEST_F(ShellSurfaceTest, AStateRequestedBeforeMappingIsAppliedOnMap)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);
    shell->setToplevel();
    shell->maximize();
    shell->rootSurface()->commit();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));

    attach(shell->rootSurface(), kOutput.size());
    shell->rootSurface()->commit();

    ASSERT_NE(nullptr, shell->window());
    EXPECT_EQ(WindowStateType::Maximized, shell->window()->stateType());
    EXPECT_EQ(kOutput, shell->window()->bounds());
}

////////////////////////////////////////////////////////////////////////////////
// Configure serials and acknowledgement

TEST_F(ShellSurfaceTest, ConfiguresAreCoalescedIntoOnePerCommit)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    const size_t before = recorder->configures.size();

    // Maximizing moves the bounds and the state type; the client sees one
    // configure, not one per change.
    shell->maximize();

    EXPECT_EQ(before + 1, recorder->configures.size());
}

TEST_F(ShellSurfaceTest, AcknowledgingDrainsEveryOlderConfigure)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);

    shell->maximize();
    shell->unmaximize();
    ASSERT_GE(shell->pendingConfigureCount(), 2u);

    const uint32_t newest = recorder->serials.back();
    EXPECT_TRUE(shell->acknowledgeConfigure(newest));
    EXPECT_EQ(0u, shell->pendingConfigureCount());
}

TEST_F(ShellSurfaceTest, AnAlreadySupersededAcknowledgementIsRejected)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);

    shell->maximize();
    shell->unmaximize();
    const uint32_t oldest = recorder->serials[recorder->serials.size() - 2];
    const uint32_t newest = recorder->serials.back();

    EXPECT_TRUE(shell->acknowledgeConfigure(newest));
    // The older serial went with it and can no longer be acknowledged.
    EXPECT_FALSE(shell->acknowledgeConfigure(oldest));
}

TEST_F(ShellSurfaceTest, AnUnknownAcknowledgementIsRejected)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));

    EXPECT_FALSE(shell->acknowledgeConfigure(0xdeadbeef));
}

TEST_F(ShellSurfaceTest, AcknowledgingTheSameSerialTwiceIsRejected)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);

    shell->maximize();
    const uint32_t serial = recorder->serials.back();

    EXPECT_TRUE(shell->acknowledgeConfigure(serial));
    EXPECT_FALSE(shell->acknowledgeConfigure(serial));
}

////////////////////////////////////////////////////////////////////////////////
// Metadata and size constraints

TEST_F(ShellSurfaceTest, TitleAndApplicationIdReachTheWindow)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));

    shell->setTitle(QStringLiteral("Editor"));
    shell->setApplicationId(QStringLiteral("org.cutefish.Editor"));

    EXPECT_EQ(QStringLiteral("Editor"), shell->window()->title());
    EXPECT_EQ(QStringLiteral("org.cutefish.Editor"), shell->window()->applicationId());
}

TEST_F(ShellSurfaceTest, TitleSetBeforeMappingSurvivesTheMap)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);
    shell->setToplevel();
    shell->setTitle(QStringLiteral("Early"));
    shell->rootSurface()->commit();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    attach(shell->rootSurface(), QSize(200, 100));
    shell->rootSurface()->commit();

    EXPECT_EQ(QStringLiteral("Early"), shell->window()->title());
}

TEST_F(ShellSurfaceTest, MinimumAndMaximumSizeApplyAtTheNextCommit)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));

    shell->setMinimumSize(QSize(120, 90));
    shell->setMaximumSize(QSize(500, 400));
    // Pending until the commit, exactly like the window geometry.
    EXPECT_EQ(QSize(1, 1), shell->window()->minimumSize());

    shell->rootSurface()->commit();

    EXPECT_EQ(QSize(120, 90), shell->window()->minimumSize());
    EXPECT_EQ(QSize(500, 400), shell->window()->maximumSize());
    EXPECT_TRUE(shell->window()->isResizable());
}

TEST_F(ShellSurfaceTest, EqualMinimumAndMaximumSizeMakesTheWindowUnresizable)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));

    shell->setMinimumSize(QSize(300, 200));
    shell->setMaximumSize(QSize(300, 200));
    shell->rootSurface()->commit();

    EXPECT_FALSE(shell->window()->isResizable());
    // And an unresizable window will not maximize.
    shell->maximize();
    EXPECT_EQ(WindowStateType::Normal, shell->window()->stateType());
}

////////////////////////////////////////////////////////////////////////////////
// State changes

TEST_F(ShellSurfaceTest, MaximizeConfiguresTheWorkAreaSize)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);

    shell->maximize();

    EXPECT_EQ(WindowStateType::Maximized, recorder->last().stateType);
    EXPECT_EQ(kOutput.size(), recorder->last().size);
}

TEST_F(ShellSurfaceTest, UnmaximizeReturnsToTheRestoreBounds)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    const QRect original = shell->window()->bounds();

    shell->maximize();
    shell->unmaximize();

    EXPECT_EQ(WindowStateType::Normal, recorder->last().stateType);
    EXPECT_EQ(original, shell->window()->bounds());
}

TEST_F(ShellSurfaceTest, FullscreenRestoresIntoTheMaximizedStateItCameFrom)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);

    shell->maximize();
    shell->setFullscreen(true);

    EXPECT_EQ(WindowStateType::Fullscreen, recorder->last().stateType);
    // What a restore would land in, which the protocol layer turns into a
    // maximized state alongside the fullscreen one.
    EXPECT_EQ(WindowStateType::Maximized, recorder->last().restoreStateType);

    shell->setFullscreen(false);
    EXPECT_EQ(WindowStateType::Maximized, recorder->last().stateType);
}

TEST_F(ShellSurfaceTest, MinimizeHidesTheWindowAndDropsActivation)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));

    shell->minimize();

    EXPECT_EQ(WindowStateType::Minimized, shell->window()->stateType());
    EXPECT_FALSE(shell->window()->isVisible());
    EXPECT_FALSE(shell->window()->isActive());
}

TEST_F(ShellSurfaceTest, ActivationChangesAreConfigured)
{
    Recorder *first = nullptr;
    Recorder *second = nullptr;
    ShellSurface *a = makeMappedToplevel(QSize(300, 200), &first);
    ShellSurface *b = makeMappedToplevel(QSize(300, 200), &second);
    ASSERT_TRUE(b->window()->isActive());

    const size_t before = first->configures.size();
    manager.activation()->activate(a->window());

    EXPECT_TRUE(first->configures.size() > before);
    EXPECT_TRUE(first->last().activated);
    EXPECT_FALSE(second->last().activated);
}

TEST_F(ShellSurfaceTest, TheResizingStateIsConfiguredForTheDurationOfADrag)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    shell->window()->setBounds(QRect(100, 100, 300, 200));

    ASSERT_TRUE(manager.eventHandler()->beginResize(shell->window(), ResizeEdge::Right,
                                                    QPointF(400, 200)));
    EXPECT_TRUE(recorder->last().resizing);

    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Success);
    EXPECT_FALSE(recorder->last().resizing);
}

////////////////////////////////////////////////////////////////////////////////
// Window geometry

TEST_F(ShellSurfaceTest, WindowGeometryDecidesTheWindowSize)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);
    shell->setToplevel();
    shell->rootSurface()->commit();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));

    // A CSD client whose buffer carries a 10px shadow all round.
    shell->setGeometry(QRect(10, 10, 300, 200));
    attach(shell->rootSurface(), QSize(320, 220));
    shell->rootSurface()->commit();

    ASSERT_NE(nullptr, shell->window());
    EXPECT_EQ(QSize(300, 200), shell->window()->bounds().size());
    // The shadow hangs outside the window, so the buffer starts above and left
    // of the window origin.
    EXPECT_EQ(QPoint(-10, -10), shell->window()->surfaceOrigin());
    EXPECT_EQ(shell->window()->bounds().topLeft() + QPoint(-10, -10), surfacePosition(shell));
}

TEST_F(ShellSurfaceTest, ClearingTheGeometryFallsBackToTheContentBounds)
{
    ShellSurface *shell = makeMappedToplevel(QSize(320, 220));
    shell->setGeometry(QRect(10, 10, 300, 200));
    shell->rootSurface()->commit();
    ASSERT_EQ(QSize(300, 200), shell->visibleBounds().size());

    shell->setGeometry(QRect());
    shell->rootSurface()->commit();

    EXPECT_EQ(QRect(0, 0, 320, 220), shell->visibleBounds());
}

TEST_F(ShellSurfaceTest, AClientResizeMovesTheWindowSizeWithIt)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    // Nothing is outstanding, so the client's own size is not second-guessing
    // a configure the compositor already sent.
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    const QPoint origin = shell->window()->bounds().topLeft();

    attach(shell->rootSurface(), QSize(400, 260));
    shell->rootSurface()->commit();

    EXPECT_EQ(QRect(origin, QSize(400, 260)), shell->window()->bounds());
}

TEST_F(ShellSurfaceTest, AResizeFromTheLeftKeepsTheContentsInPlaceUntilTheClientCatchesUp)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    Window *window = shell->window();
    window->setBounds(QRect(400, 100, 300, 200));
    ASSERT_EQ(QPoint(400, 100), surfacePosition(shell));

    ASSERT_TRUE(manager.eventHandler()->beginResize(window, ResizeEdge::Left, QPointF(400, 200)));
    // Drag the left edge 50px to the left: the window grows and its origin
    // moves, but the client is still painting 300x200.
    manager.eventHandler()->onPointerMotion(QPointF(350, 200));
    EXPECT_EQ(QRect(350, 100, 350, 200), window->bounds());
    EXPECT_EQ(QPoint(400, 100), surfacePosition(shell));

    // The client acknowledges and commits the new size.
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    attach(shell->rootSurface(), QSize(350, 200));
    shell->rootSurface()->commit();

    EXPECT_EQ(QPoint(350, 100), surfacePosition(shell));
}

TEST_F(ShellSurfaceTest, ResizeAnchorsCommittedPixelsWhenGeometryRunsAhead)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    Window *window = shell->window();
    window->setBounds(QRect(400, 100, 300, 200));
    shell->setGeometry({0, 0, 300, 200});
    shell->rootSurface()->commit();
    ASSERT_TRUE(manager.eventHandler()->beginResize(window, ResizeEdge::TopLeft, {400, 100}));

    for (const QSize target : {QSize(350, 250), QSize(280, 180)}) {
        // A resize takes one step per frame.
        Q_EMIT manager.compositor()->frameStarted(0);
        manager.eventHandler()->onPointerMotion(
                QPointF(700 - target.width(), 300 - target.height()));
        ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
        shell->setGeometry(QRect(QPoint(), target));
        attach(shell->rootSurface(), {300, 200});
        shell->rootSurface()->commit();
        EXPECT_EQ(surfacePosition(shell) + QPoint(300, 200), QPoint(700, 300));

        attach(shell->rootSurface(), target);
        shell->rootSurface()->commit();
        EXPECT_EQ(surfacePosition(shell) + QPoint(target.width(), target.height()),
                  QPoint(700, 300));
    }
}

TEST_F(ShellSurfaceTest, AResizeFromTheRightLeavesTheOriginAlone)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    Window *window = shell->window();
    window->setBounds(QRect(400, 100, 300, 200));

    ASSERT_TRUE(manager.eventHandler()->beginResize(window, ResizeEdge::Right, QPointF(700, 200)));
    manager.eventHandler()->onPointerMotion(QPointF(750, 200));

    EXPECT_EQ(QRect(400, 100, 350, 200), window->bounds());
    EXPECT_EQ(QPoint(400, 100), surfacePosition(shell));
}

////////////////////////////////////////////////////////////////////////////////
// Transients

TEST_F(ShellSurfaceTest, SetParentMapsOntoTheTransientRelationship)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    ShellSurface *child = makeMappedToplevel(QSize(200, 150));

    EXPECT_TRUE(child->setTransientParent(parent));

    EXPECT_EQ(parent->window(), child->window()->transientParent());
    ASSERT_EQ(1u, parent->window()->transientChildren().size());
    EXPECT_EQ(child->window(), parent->window()->transientChildren().front());
}

TEST_F(ShellSurfaceTest, SetParentBeforeMappingIsAppliedOnMap)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));

    Recorder *recorder = nullptr;
    ShellSurface *child = makeShell(&recorder);
    child->setToplevel();
    EXPECT_TRUE(child->setTransientParent(parent));
    child->rootSurface()->commit();
    ASSERT_TRUE(child->acknowledgeConfigure(recorder->serials.back()));
    attach(child->rootSurface(), QSize(200, 150));
    child->rootSurface()->commit();

    EXPECT_EQ(parent->window(), child->window()->transientParent());
}

TEST_F(ShellSurfaceTest, SetParentRefusesACycle)
{
    ShellSurface *a = makeMappedToplevel(QSize(400, 300));
    ShellSurface *b = makeMappedToplevel(QSize(200, 150));
    ASSERT_TRUE(b->setTransientParent(a));

    EXPECT_FALSE(a->setTransientParent(a));
    EXPECT_FALSE(a->setTransientParent(b));
}

////////////////////////////////////////////////////////////////////////////////
// Unmap, remap and destruction

TEST_F(ShellSurfaceTest, CommittingANullBufferUnmapsTheWindow)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));
    ASSERT_EQ(1u, manager.windows().size());

    shell->rootSurface()->attach(nullptr, {});
    shell->rootSurface()->commit();

    EXPECT_EQ(nullptr, shell->window());
    EXPECT_TRUE(manager.windows().empty());
}

TEST_F(ShellSurfaceTest, ARemapStartsTheConfigureSequenceOver)
{
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200), &recorder);
    shell->rootSurface()->attach(nullptr, {});
    shell->rootSurface()->commit();
    const size_t afterUnmap = recorder->configures.size();

    // An empty commit asks for a fresh initial configure.
    shell->rootSurface()->commit();
    ASSERT_EQ(afterUnmap + 1, recorder->configures.size());
    EXPECT_FALSE(recorder->last().activated);
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));

    attach(shell->rootSurface(), QSize(300, 200));
    shell->rootSurface()->commit();

    ASSERT_NE(nullptr, shell->window());
    EXPECT_EQ(1u, manager.windows().size());
}

TEST_F(ShellSurfaceTest, DestroyingTheSurfaceDestroysTheWindow)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));
    compositor::Surface *surface = shell->rootSurface();
    ASSERT_EQ(1u, manager.windows().size());

    takeSurface(surface).reset();

    EXPECT_EQ(nullptr, shell->rootSurface());
    EXPECT_EQ(nullptr, shell->window());
    EXPECT_TRUE(manager.windows().empty());
}

TEST_F(ShellSurfaceTest, DestroyingTheShellSurfaceDestroysTheWindow)
{
    makeMappedToplevel(QSize(300, 200));
    ASSERT_EQ(1u, manager.windows().size());

    takeShell(shells.back().get()).reset();

    EXPECT_TRUE(manager.windows().empty());
}

TEST_F(ShellSurfaceTest, ANewToplevelCanBeCreatedWhileAnotherIsBeingDestroyed)
{
    ShellSurface *victim = makeMappedToplevel(QSize(300, 200));
    ShellSurface *survivor = makeMappedToplevel(QSize(200, 100));

    // Creating a window from inside the removal notification is the shape a
    // client hits when one toplevel closes and another opens at once.
    bool created = false;
    const QMetaObject::Connection connection =
            QObject::connect(&manager, &WindowManager::windowRemoved, &manager, [&](Window *) {
                if (created)
                    return;
                created = true;
                makeMappedToplevel(QSize(150, 150));
            });

    takeShell(victim).reset();
    QObject::disconnect(connection);

    EXPECT_TRUE(created);
    EXPECT_EQ(2u, manager.windows().size());
    EXPECT_NE(nullptr, survivor->window());
}

TEST_F(ShellSurfaceTest, TheShellSurfaceMayBeDestroyedFromInsideAConfigureCallback)
{
    ShellSurface *shell = makeMappedToplevel(QSize(300, 200));

    // A configure callback writes to the client and flushes, which in the
    // protocol layer can take the resource that owns the shell surface with it.
    std::unique_ptr<ShellSurface> owned;
    shell->setConfigureCallback([&](const ShellSurface::ConfigureState &) -> uint32_t {
        owned = takeShell(shell);
        owned.reset();
        return 0;
    });

    shell->maximize();

    EXPECT_TRUE(manager.windows().empty());
}

TEST_F(ShellSurfaceTest, TheShellSurfaceMayBeDestroyedFromInsideAnActivationConfigure)
{
    ShellSurface *first = makeMappedToplevel(QSize(300, 200));
    makeMappedToplevel(QSize(200, 150));

    std::unique_ptr<ShellSurface> owned;
    first->setConfigureCallback([&](const ShellSurface::ConfigureState &) -> uint32_t {
        owned = takeShell(first);
        owned.reset();
        return 0;
    });

    // Activating it configures it, and the configure destroys it from under the
    // activation walk.
    manager.activation()->activate(first->window());

    EXPECT_EQ(1u, manager.windows().size());
}

////////////////////////////////////////////////////////////////////////////////
// Popups

TEST_F(ShellSurfaceTest, APopupIsPlacedRelativeToItsParentWindowGeometry)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    parent->window()->setBounds(QRect(200, 150, 400, 300));

    Recorder *recorder = nullptr;
    ShellSurface *popup = makeShell(&recorder);
    ASSERT_TRUE(popup->setPopup(parent, QRect(20, 40, 100, 80)));
    popup->configurePopup();
    ASSERT_EQ(1u, recorder->popupConfigures.size());
    EXPECT_EQ(QRect(20, 40, 100, 80), recorder->popupConfigures.front());
    ASSERT_TRUE(popup->acknowledgeConfigure(recorder->serials.back()));

    attach(popup->rootSurface(), QSize(100, 80));
    popup->rootSurface()->commit();

    ASSERT_NE(nullptr, popup->window());
    EXPECT_TRUE(popup->window()->isPopup());
    EXPECT_EQ(parent->window(), popup->window()->transientParent());
    EXPECT_EQ(parent->window(), popup->ownerWindow());
    EXPECT_EQ(QPoint(220, 190), surfacePosition(popup));
    EXPECT_EQ(popup->window(), manager.windowForSurface(popup->rootSurface()));
}

TEST_F(ShellSurfaceTest, FractionalOutputKeepsPopupCoordinatesAndIndependentViewport)
{
    compositor.setOutputScale(*compositor::OutputScale::parse("1.5"));
    manager.setOutputBounds(QRect(0, 0, 1280, 720));
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    parent->window()->setBounds(QRect(200, 150, 400, 300));

    Recorder *recorder = nullptr;
    ShellSurface *popup = makeShell(&recorder);
    ASSERT_TRUE(popup->setPopup(parent, QRect(20, 40, 100, 80)));
    popup->configurePopup();
    ASSERT_EQ(1u, recorder->popupConfigures.size());
    EXPECT_EQ(QRect(20, 40, 100, 80), recorder->popupConfigures.front());
    ASSERT_TRUE(popup->acknowledgeConfigure(recorder->serials.back()));

    attach(popup->rootSurface(), QSize(150, 120));
    popup->rootSurface()->setViewport(QSize(100, 80));
    popup->rootSurface()->commit();

    EXPECT_EQ(popup->rootSurface()->preferredScale(), 180u);
    EXPECT_EQ(popup->rootSurface()->contentSize(), QSize(100, 80));
    ASSERT_NE(nullptr, popup->window());
    EXPECT_TRUE(popup->window()->isPopup());
    EXPECT_EQ(parent->window(), popup->window()->transientParent());
    EXPECT_EQ(parent->window(), popup->ownerWindow());
    EXPECT_EQ(QPoint(220, 190), surfacePosition(popup));
    EXPECT_EQ(popup->window(), manager.windowForSurface(popup->rootSurface()));
}

TEST_F(ShellSurfaceTest, APopupFollowsItsParentWhenTheWindowMoves)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    parent->window()->setBounds(QRect(200, 150, 400, 300));

    Recorder *recorder = nullptr;
    ShellSurface *popup = makeShell(&recorder);
    ASSERT_TRUE(popup->setPopup(parent, QRect(20, 40, 100, 80)));
    attach(popup->rootSurface(), QSize(100, 80));
    popup->rootSurface()->commit();
    ASSERT_EQ(QPoint(220, 190), surfacePosition(popup));

    parent->window()->setBounds(QRect(300, 250, 400, 300));

    EXPECT_EQ(QPoint(320, 290), surfacePosition(popup));
}

TEST_F(ShellSurfaceTest, ANestedPopupAccumulatesItsParentsOffsets)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    parent->window()->setBounds(QRect(100, 100, 400, 300));

    ShellSurface *menu = makeShell();
    ASSERT_TRUE(menu->setPopup(parent, QRect(10, 20, 100, 80)));
    attach(menu->rootSurface(), QSize(100, 80));
    menu->rootSurface()->commit();

    ShellSurface *submenu = makeShell();
    ASSERT_TRUE(submenu->setPopup(menu, QRect(5, 7, 60, 40)));
    attach(submenu->rootSurface(), QSize(60, 40));
    submenu->rootSurface()->commit();

    EXPECT_EQ(QPoint(115, 127), surfacePosition(submenu));
    EXPECT_EQ(parent->window(), submenu->ownerWindow());
}

TEST_F(ShellSurfaceTest, APopupRefusesAParentCycle)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    ShellSurface *menu = makeShell();
    ASSERT_TRUE(menu->setPopup(parent, QRect(0, 0, 10, 10)));

    ShellSurface *other = makeShell();
    EXPECT_FALSE(other->setPopup(other, QRect(0, 0, 10, 10)));
}

TEST_F(ShellSurfaceTest, RepositionMovesThePopupAndConfiguresItAgain)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    parent->window()->setBounds(QRect(100, 100, 400, 300));

    Recorder *recorder = nullptr;
    ShellSurface *popup = makeShell(&recorder);
    ASSERT_TRUE(popup->setPopup(parent, QRect(10, 20, 100, 80)));
    attach(popup->rootSurface(), QSize(100, 80));
    popup->rootSurface()->commit();

    popup->reposition(QRect(50, 60, 100, 80));

    EXPECT_EQ(QPoint(150, 160), surfacePosition(popup));
    ASSERT_EQ(1u, recorder->popupConfigures.size());
    EXPECT_EQ(QRect(50, 60, 100, 80), recorder->popupConfigures.back());
}

TEST_F(ShellSurfaceTest, APressOutsideAGrabbingPopupDismissesTheChainAndIsNotDelivered)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    parent->window()->setBounds(QRect(0, 0, 400, 300));

    Recorder *menuRecorder = nullptr;
    ShellSurface *menu = makeShell(&menuRecorder);
    ASSERT_TRUE(menu->setPopup(parent, QRect(10, 20, 100, 80)));
    menu->grab();
    attach(menu->rootSurface(), QSize(100, 80));
    menu->rootSurface()->commit();
    compositor.updateTextures(renderer);
    ASSERT_EQ(1u, manager.popupGrabs().size());

    // Inside the menu: delivered, and the grab stands.
    EXPECT_FALSE(manager.onPointerButton(at(1), QPointF(50, 60), BTN_LEFT, true));
    EXPECT_EQ(0, menuRecorder->closes);

    // Outside it: consumed, and the menu is told to go away.
    EXPECT_TRUE(manager.onPointerButton(at(2), QPointF(300, 250), BTN_LEFT, true));
    EXPECT_EQ(1, menuRecorder->closes);
    EXPECT_TRUE(manager.popupGrabs().empty());
}

TEST_F(ShellSurfaceTest, DestroyingAPopupParentLeavesTheChainSafeToTearDown)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    ShellSurface *menu = makeShell();
    ASSERT_TRUE(menu->setPopup(parent, QRect(10, 20, 100, 80)));
    menu->grab();
    attach(menu->rootSurface(), QSize(100, 80));
    menu->rootSurface()->commit();

    // The parent's surface goes first, as it does when a client exits.
    compositor::Surface *parentSurface = parent->rootSurface();
    takeSurface(parentSurface).reset();

    EXPECT_EQ(nullptr, menu->ownerWindow());
    EXPECT_TRUE(manager.windows().empty());
    // Tearing the popup down afterwards must not touch the dead parent.
    shells.clear();
}

TEST_F(ShellSurfaceTest, DismissingAGrabChainSurvivesTheWholeChainBeingDestroyed)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    parent->window()->setBounds(QRect(0, 0, 400, 300));

    ShellSurface *menu = makeShell();
    ASSERT_TRUE(menu->setPopup(parent, QRect(10, 20, 100, 80)));
    menu->grab();
    attach(menu->rootSurface(), QSize(100, 80));
    menu->rootSurface()->commit();

    ShellSurface *submenu = makeShell();
    ASSERT_TRUE(submenu->setPopup(menu, QRect(5, 5, 60, 40)));
    submenu->grab();
    attach(submenu->rootSurface(), QSize(60, 40));
    submenu->rootSurface()->commit();
    compositor.updateTextures(renderer);
    ASSERT_EQ(2u, manager.popupGrabs().size());

    // A real toolkit tears its whole menu chain down on the first popup_done.
    std::unique_ptr<ShellSurface> ownedMenu;
    std::unique_ptr<ShellSurface> ownedSubmenu;
    submenu->setCloseCallback([&] {
        ownedSubmenu = takeShell(submenu);
        ownedMenu = takeShell(menu);
        ownedSubmenu.reset();
        ownedMenu.reset();
    });

    EXPECT_TRUE(manager.onPointerButton(at(1), QPointF(350, 250), BTN_LEFT, true));

    EXPECT_TRUE(manager.popupGrabs().empty());
    EXPECT_EQ(1u, manager.windows().size());
}

TEST_F(ShellSurfaceTest, AGrabbingPopupLeavesTheGrabChainWhenItUnmaps)
{
    ShellSurface *parent = makeMappedToplevel(QSize(400, 300));
    ShellSurface *menu = makeShell();
    ASSERT_TRUE(menu->setPopup(parent, QRect(10, 20, 100, 80)));
    menu->grab();
    attach(menu->rootSurface(), QSize(100, 80));
    menu->rootSurface()->commit();
    ASSERT_EQ(1u, manager.popupGrabs().size());

    menu->rootSurface()->attach(nullptr, {});
    menu->rootSurface()->commit();

    EXPECT_TRUE(manager.popupGrabs().empty());
}

} // namespace

TEST_F(ShellSurfaceTest, BoundsAnimationWaitsForCommitAfterLatestConfigureAcknowledgement)
{
    class SnapshotRenderer : public testsupport::FakeRenderer
    {
    public:
        int captures = 0;

        bool renderToTexture(std::unique_ptr<render::Texture> &texture, const QRect &bounds,
                             const std::function<void()> &) override
        {
            ++captures;
            std::vector<uint8_t> pixels(bounds.width() * bounds.height() * 4);
            texture = createTexture(bounds.size(), pixels.data(), bounds.width() * 4,
                                    render::PixelFormat::Bgrx8888);
            return bool(texture);
        }
    } snapshots;

    Recorder *recorder = nullptr;
    auto *shell = makeMappedToplevel({400, 300}, &recorder);
    auto *animation = dynamic_cast<scene::WindowAnimation *>(shell->window()->node());
    ASSERT_NE(animation, nullptr);
    animation->finish();
    animation->render(snapshots);
    shell->maximize();
    EXPECT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    attach(shell->rootSurface(), kOutput.size());
    shell->rootSurface()->commit();
    animation->finish();
    animation->render(snapshots);
    const auto original = snapshots.calls.back();
    const int captures = snapshots.captures;

    shell->unmaximize();
    const auto firstSerial = recorder->serials.back();
    manager.activation()->deactivate(shell->window());
    ASSERT_NE(firstSerial, recorder->serials.back());
    EXPECT_TRUE(shell->acknowledgeConfigure(firstSerial));
    shell->rootSurface()->commit();
    animation->render(snapshots);
    EXPECT_EQ(snapshots.captures, captures);
    EXPECT_EQ(snapshots.calls.back().rect, original.rect);

    EXPECT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    animation->render(snapshots);
    EXPECT_EQ(snapshots.captures, captures);
    const QRect restoredBounds = shell->window()->bounds();
    shell->rootSurface()->commit(); // State-only commit, still the maximized buffer.
    animation->render(snapshots);
    EXPECT_EQ(snapshots.captures, captures);
    EXPECT_EQ(shell->window()->bounds(), restoredBounds);
    EXPECT_EQ(snapshots.calls.back().rect, original.rect);
    auto resizedBuffer =
            pool->createBuffer({400, 300}, 0, 400 * 4, render::PixelFormat::Bgrx8888, nullptr);
    shell->rootSurface()->attach(resizedBuffer, {});
    shell->rootSurface()->commit();
    animation->render(snapshots);
    EXPECT_EQ(snapshots.captures, captures + 1);
    EXPECT_EQ(snapshots.calls.back().texture, original.texture);
    EXPECT_TRUE(original.rect.contains(snapshots.calls.back().rect));
}

// Dragging a maximized window out by its title bar changes its bounds in one
// step, which is a cross-fade like any other state change: nothing may be drawn
// at a size the client has not reached, or the frame or two before it catches
// up flashes - a stale buffer stretched into the new bounds, with the exterior
// decoration of a restored window around it.
TEST_F(ShellSurfaceTest, DraggingOutOfMaximizedHoldsTheCrossFadeUntilTheClientRedraws)
{
    Recorder *recorder = nullptr;
    auto *shell = makeMappedToplevel({400, 300}, &recorder);
    auto *animation = dynamic_cast<scene::WindowAnimation *>(shell->window()->node());
    ASSERT_NE(animation, nullptr);

    shell->maximize();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    auto maximizedBuffer = pool->createBuffer(kOutput.size(), 0, kOutput.width() * 4,
                                              render::PixelFormat::Bgrx8888, nullptr);
    shell->setGeometry({QPoint(), kOutput.size()});
    shell->rootSurface()->attach(maximizedBuffer, {});
    shell->rootSurface()->commit();
    ASSERT_FALSE(animation->isWaitingForBoundsContent());

    Window *window = shell->window();
    ASSERT_TRUE(manager.eventHandler()->beginMove(window, QPointF(kOutput.width() / 2, 10)));
    manager.eventHandler()->onPointerMotion(QPointF(kOutput.width() / 2, 300));
    ASSERT_LT(window->bounds().width(), kOutput.width());
    EXPECT_TRUE(animation->isWaitingForBoundsContent());

    // Another frame of the maximized buffer, and another pointer motion, do not
    // release it: the client has acknowledged nothing yet.
    shell->rootSurface()->attach(maximizedBuffer, {});
    shell->rootSurface()->commit();
    manager.eventHandler()->onPointerMotion(QPointF(kOutput.width() / 2 + 40, 320));
    EXPECT_TRUE(animation->isWaitingForBoundsContent());

    // The commit that carries the restored size is what starts the cross-fade.
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    shell->setGeometry({0, 0, 400, 300});
    auto restoredBuffer =
            pool->createBuffer({400, 300}, 0, 400 * 4, render::PixelFormat::Bgrx8888, nullptr);
    shell->rootSurface()->attach(restoredBuffer, {});
    shell->rootSurface()->commit();
    EXPECT_FALSE(animation->isWaitingForBoundsContent());

    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Success);
    EXPECT_EQ(WindowStateType::Normal, window->stateType());
    EXPECT_FALSE(animation->isWaitingForBoundsContent());
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    shell->rootSurface()->commit();
    EXPECT_FALSE(animation->isWaitingForBoundsContent());
}

TEST_F(ShellSurfaceTest, RestoredBufferDoesNotWaitForLaterActivationConfigure)
{
    Recorder *recorder = nullptr;
    auto *shell = makeMappedToplevel({400, 300}, &recorder);
    auto *window = shell->window();
    auto *animation = static_cast<scene::WindowAnimation *>(window->node());
    shell->maximize();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    auto maximizedBuffer = pool->createBuffer(kOutput.size(), 0, kOutput.width() * 4,
                                              render::PixelFormat::Bgrx8888, nullptr);
    shell->rootSurface()->attach(maximizedBuffer, {});
    shell->rootSurface()->commit();
    ASSERT_TRUE(manager.eventHandler()->beginMove(window, QPointF(kOutput.width() / 2, 10)));
    manager.eventHandler()->onPointerMotion(QPointF(kOutput.width() / 2, 300));
    const auto restoreSerial = recorder->serials.back();
    ASSERT_EQ(recorder->configures.back().stateType, WindowStateType::Normal);
    ASSERT_EQ(recorder->configures.back().size, QSize(400, 300));
    manager.activation()->deactivate(window);
    ASSERT_NE(restoreSerial, recorder->serials.back());
    ASSERT_TRUE(shell->acknowledgeConfigure(restoreSerial));
    auto restoredBuffer =
            pool->createBuffer({400, 300}, 0, 400 * 4, render::PixelFormat::Bgrx8888, nullptr);
    shell->rootSurface()->attach(restoredBuffer, {});
    shell->rootSurface()->commit();
    ASSERT_EQ(shell->rootSurface()->contentSize(), QSize(400, 300));
    ASSERT_TRUE(shell->hasPendingConfigures());
    EXPECT_FALSE(animation->isWaitingForBoundsContent());
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Success);
    EXPECT_FALSE(animation->isWaitingForBoundsContent());
}

TEST_F(ShellSurfaceTest, DragReleaseBeforeCsdRedrawKeepsWaitingUntilRestoredBuffer)
{
    Recorder *recorder = nullptr;
    auto *shell = makeMappedToplevel({400, 300}, &recorder);
    auto *window = shell->window();
    auto *animation = static_cast<scene::WindowAnimation *>(window->node());
    shell->maximize();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    auto maximizedBuffer = pool->createBuffer(kOutput.size(), 0, kOutput.width() * 4,
                                              render::PixelFormat::Bgrx8888, nullptr);
    shell->setGeometry(QRect(QPoint(), kOutput.size()));
    shell->rootSurface()->attach(maximizedBuffer, {});
    shell->rootSurface()->commit();
    ASSERT_TRUE(manager.eventHandler()->beginMove(window, QPointF(kOutput.width() / 2, 10)));
    manager.eventHandler()->onPointerMotion(QPointF(kOutput.width() / 2, 300));
    const auto restoreSerial = recorder->serials.back();
    manager.eventHandler()->completeDrag(ToplevelEventHandler::DragResult::Success);
    ASSERT_TRUE(shell->acknowledgeConfigure(restoreSerial));
    shell->rootSurface()->commit();
    EXPECT_TRUE(animation->isWaitingForBoundsContent());
    shell->rootSurface()->attach(maximizedBuffer, {});
    shell->rootSurface()->commit();
    EXPECT_TRUE(animation->isWaitingForBoundsContent());

    auto restoredBuffer =
            pool->createBuffer({424, 324}, 0, 424 * 4, render::PixelFormat::Bgrx8888, nullptr);
    shell->setGeometry({12, 12, 400, 300});
    shell->rootSurface()->attach(restoredBuffer, {});
    shell->rootSurface()->commit();
    EXPECT_FALSE(animation->isWaitingForBoundsContent());
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    shell->rootSurface()->commit();
    EXPECT_FALSE(animation->isWaitingForBoundsContent());
}

TEST_F(ShellSurfaceTest, ReversingRestoreCanReuseTheStillMaximizedBuffer)
{
    Recorder *recorder = nullptr;
    auto *shell = makeMappedToplevel({400, 300}, &recorder);
    auto *animation = dynamic_cast<scene::WindowAnimation *>(shell->window()->node());
    ASSERT_NE(animation, nullptr);
    shell->maximize();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    auto buffer = pool->createBuffer(kOutput.size(), 0, kOutput.width() * 4,
                                     render::PixelFormat::Bgrx8888, nullptr);
    shell->setGeometry({QPoint(), kOutput.size()});
    shell->rootSurface()->attach(buffer, {});
    shell->rootSurface()->commit();
    EXPECT_FALSE(animation->isWaitingForBoundsContent());

    shell->unmaximize();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    shell->setGeometry({0, 0, 400, 300});
    shell->rootSurface()->attach(buffer, {});
    shell->rootSurface()->commit();
    EXPECT_TRUE(animation->isWaitingForBoundsContent());

    shell->maximize();
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    shell->setGeometry({QPoint(), kOutput.size()});
    shell->rootSurface()->attach(buffer, {});
    shell->rootSurface()->commit();
    EXPECT_FALSE(animation->isWaitingForBoundsContent());
}

TEST_F(ShellSurfaceTest, AGlassBodyInsideTheSurfaceCastsTheShadow)
{
    ShadowController shadows(&manager, &shadowProvider, &shadowDelegate);
    auto *banner = makeMappedToplevel(QSize(396, 116));
    banner->window()->setRole(WindowRole::Notification);
    auto *surface = banner->rootSurface();
    surface->setBackgroundBlur(QRect(0, 0, 396, 116), 18);
    surface->commit();
    auto *shadow = dynamic_cast<decoration::Shadow *>(shadows.shadowForWindow(banner->window()));
    ASSERT_NE(nullptr, shadow);
    const QRect whole = shadow->shadowBounds();
    ASSERT_FALSE(whole.isEmpty());

    // Room kept around the body for something overhanging its corner.
    surface->setBackgroundBlur(QRect(8, 8, 380, 100), 18);
    surface->commit();
    EXPECT_EQ(whole.adjusted(8, 8, -8, -8), shadow->shadowBounds());
    EXPECT_EQ(18, shadow->node()->radius());
}

TEST_F(ShellSurfaceTest, PopupUsesTheSharedShadowControllerAndTransientStacking)
{
    ShadowController shadows(&manager, &shadowProvider, &shadowDelegate);
    auto *desktop = makeMappedToplevel(kOutput.size());
    desktop->window()->setRole(WindowRole::Desktop);
    auto *dock = makeMappedToplevel(QSize(400, 60));
    dock->window()->setRole(WindowRole::Dock);
    dock->window()->setBounds(QRect(300, 740, 400, 60));
    auto *popup = makeShell();
    ASSERT_TRUE(popup->setPopup(desktop, QRect(20, 30, 160, 120)));
    auto *surface = popup->rootSurface();
    surface->setBackgroundBlur(QRect(0, 0, 160, 120), 8);
    attach(surface, QSize(160, 120));
    surface->commit();
    auto *window = popup->window();
    ASSERT_NE(nullptr, window);
    ASSERT_EQ(desktop->window(), window->transientParent());
    EXPECT_FALSE(window->isActive());
    surface->commit();
    EXPECT_FALSE(window->isResizable());
    window->maximize();
    window->minimize();
    EXPECT_EQ(WindowStateType::Normal, window->stateType());
    EXPECT_FALSE(manager.eventHandler()->beginMove(window, QPointF(20, 30)));
    auto *shadow = dynamic_cast<decoration::Shadow *>(shadows.shadowForWindow(window));
    ASSERT_NE(nullptr, shadow);
    EXPECT_EQ(ShadowElevation::MenuOrTooltip, shadow->elevation());
    EXPECT_EQ(8, shadow->node()->radius());
    EXPECT_EQ(shadow->node(), window->node()->children().front());
    const QRect oldBounds = shadow->shadowBounds();
    popup->reposition(QRect(40, 60, 160, 120));
    EXPECT_EQ(oldBounds.translated(20, 30), shadow->shadowBounds());
    auto *submenu = makeShell();
    ASSERT_TRUE(submenu->setPopup(popup, QRect(140, 10, 100, 80)));
    attach(submenu->rootSurface(), QSize(100, 80));
    submenu->rootSurface()->commit();
    manager.raise(desktop->window());
    const auto &order = manager.windows();
    EXPECT_LT(std::find(order.begin(), order.end(), window),
              std::find(order.begin(), order.end(), submenu->window()));
    EXPECT_LT(std::find(order.begin(), order.end(), submenu->window()),
              std::find(order.begin(), order.end(), dock->window()));
    const auto &sceneOrder = surfaceContainer->children();
    EXPECT_LT(std::find(sceneOrder.begin(), sceneOrder.end(), submenu->window()->node()),
              std::find(sceneOrder.begin(), sceneOrder.end(), dock->window()->node()));
    popup->clearRole();
    EXPECT_EQ(nullptr, popup->window());
    EXPECT_EQ(nullptr, submenu->window());
    EXPECT_FALSE(surface->node()->isVisible());
}

TEST_F(ShellSurfaceTest, PopupClientShadowSuppressesTheSharedCompositorShadow)
{
    ShadowController shadows(&manager, &shadowProvider, &shadowDelegate);
    auto *parent = makeMappedToplevel(QSize(400, 300));
    auto *popup = makeShell();
    ASSERT_TRUE(popup->setPopup(parent, QRect(20, 30, 160, 120)));
    auto *surface = popup->rootSurface();
    popup->setGeometry(QRect(10, 10, 160, 120));
    attach(surface, QSize(180, 140));
    surface->commit();
    auto *shadow = shadows.shadowForWindow(popup->window());
    ASSERT_NE(nullptr, shadow);
    EXPECT_EQ(ShadowElevation::None, shadow->elevation());
    popup->setGeometry(QRect(0, 0, 180, 140));
    surface->commit();
    EXPECT_EQ(ShadowElevation::MenuOrTooltip, shadow->elevation());
    parent->window()->setVisible(false);
    EXPECT_FALSE(popup->window()->isVisible());
    EXPECT_EQ(ShadowElevation::None, shadow->elevation());
    parent->window()->setVisible(true);
    EXPECT_EQ(ShadowElevation::MenuOrTooltip, shadow->elevation());
    takeSurface(surface).reset();
    EXPECT_EQ(nullptr, popup->window());
    takeShell(popup).reset();
}

////////////////////////////////////////////////////////////////////////////////
// Transient parent lifetime

namespace {

void unmap(ShellSurface *shell)
{
    shell->rootSurface()->attach(nullptr, {});
    shell->rootSurface()->commit();
}

} // namespace

TEST_F(ShellSurfaceTest, AnUnmappedParentCountsAsNoParentAndIsNotRestoredLater)
{
    Recorder *recorder = nullptr;
    ShellSurface *parent = makeShell(&recorder);
    parent->setToplevel();
    ShellSurface *child = makeMappedToplevel(QSize(200, 100));

    EXPECT_TRUE(child->setTransientParent(parent));
    EXPECT_EQ(nullptr, child->transientParent());
    EXPECT_EQ(nullptr, child->window()->transientParent());

    parent->rootSurface()->commit();
    ASSERT_TRUE(parent->acknowledgeConfigure(recorder->serials.back()));
    attach(parent->rootSurface(), QSize(300, 200));
    parent->rootSurface()->commit();
    ASSERT_TRUE(parent->window());
    EXPECT_EQ(nullptr, child->transientParent());
    EXPECT_EQ(nullptr, child->window()->transientParent());
}

TEST_F(ShellSurfaceTest, UnmappingAParentPromotesItsChildrenForGood)
{
    ShellSurface *grandparent = makeMappedToplevel(QSize(400, 300));
    Recorder *parentRecorder = nullptr;
    ShellSurface *parent = makeMappedToplevel(QSize(300, 200), &parentRecorder);
    ShellSurface *child = makeMappedToplevel(QSize(200, 100));
    ASSERT_TRUE(parent->setTransientParent(grandparent));
    ASSERT_TRUE(child->setTransientParent(parent));
    ASSERT_EQ(parent->window(), child->window()->transientParent());

    unmap(parent);
    EXPECT_EQ(grandparent, child->transientParent());
    EXPECT_EQ(grandparent->window(), child->window()->transientParent());
    EXPECT_EQ(grandparent, parent->transientParent());

    parent->rootSurface()->commit();
    ASSERT_TRUE(parent->acknowledgeConfigure(parentRecorder->serials.back()));
    attach(parent->rootSurface(), QSize(300, 200));
    parent->rootSurface()->commit();
    ASSERT_TRUE(parent->window());
    EXPECT_EQ(grandparent->window(), parent->window()->transientParent());
    EXPECT_EQ(grandparent, child->transientParent());
    EXPECT_EQ(grandparent->window(), child->window()->transientParent());
}

TEST_F(ShellSurfaceTest, UnmappingARootParentUnparentsItsChildren)
{
    ShellSurface *parent = makeMappedToplevel(QSize(300, 200));
    ShellSurface *child = makeMappedToplevel(QSize(200, 100));
    ASSERT_TRUE(child->setTransientParent(parent));

    unmap(parent);
    EXPECT_EQ(nullptr, child->transientParent());
    EXPECT_EQ(nullptr, child->window()->transientParent());
}

TEST_F(ShellSurfaceTest, DestroyingTheRoleOrTheShellLeavesNoDanglingParent)
{
    ShellSurface *parent = makeMappedToplevel(QSize(300, 200));
    Recorder *pendingRecorder = nullptr;
    ShellSurface *pending = makeShell(&pendingRecorder);
    pending->setToplevel();
    ASSERT_TRUE(pending->setTransientParent(parent));
    ASSERT_EQ(parent, pending->transientParent());

    parent->clearRole();
    EXPECT_EQ(nullptr, pending->transientParent());
    // Mapping afterwards finds no parent to reach through.
    pending->rootSurface()->commit();
    ASSERT_TRUE(pending->acknowledgeConfigure(pendingRecorder->serials.back()));
    attach(pending->rootSurface(), QSize(200, 100));
    pending->rootSurface()->commit();
    ASSERT_TRUE(pending->window());
    EXPECT_EQ(nullptr, pending->window()->transientParent());

    ShellSurface *doomedParent = makeMappedToplevel(QSize(300, 200));
    ShellSurface *child = makeMappedToplevel(QSize(200, 100));
    ASSERT_TRUE(child->setTransientParent(doomedParent));
    takeShell(doomedParent).reset();
    EXPECT_EQ(nullptr, child->transientParent());
    EXPECT_EQ(nullptr, child->window()->transientParent());

    // A child that goes first leaves nothing behind in its parent.
    ShellSurface *survivor = makeMappedToplevel(QSize(300, 200));
    ShellSurface *doomedChild = makeMappedToplevel(QSize(200, 100));
    ASSERT_TRUE(doomedChild->setTransientParent(survivor));
    takeShell(doomedChild).reset();
    unmap(survivor);
    survivor->clearRole();
}

////////////////////////////////////////////////////////////////////////////////
// Output for new windows

namespace {

display::DisplayId displayId(const char *value)
{
    return display::DisplayId(QString::fromLatin1(value));
}

display::DisplaySnapshot physicalDisplay(const char *name, QSize size)
{
    display::DisplaySnapshot result;
    result.sessionId = displayId(name);
    result.persistentKey = QStringLiteral("edid-") + QString::fromLatin1(name);
    result.portKey = QStringLiteral("port-") + QString::fromLatin1(name);
    result.connectorName = QString::fromLatin1(name);
    result.devicePath = QStringLiteral("/dev/dri/card0");
    result.physicalSizeMm = {600, 340};
    result.modes = {{size, 60000, 148500, size.width() + 100, size.height() + 50, 0,
                     QStringLiteral("mode"), true}};
    result.currentMode = result.modes.first();
    return result;
}

display::LogicalDisplay logicalDisplay(const char *name, QRect bounds, bool primary)
{
    return {displayId(name),
            {displayId(name)},
            QStringLiteral("Cutefish-") + QString::fromLatin1(name),
            bounds,
            bounds.adjusted(0, 24, 0, 0),
            {},
            display::DisplayRotation::Normal,
            primary};
}

constexpr QRect kPrimary(0, 0, 1000, 800);
constexpr QRect kSecondary(1000, 0, 800, 600);
constexpr QRect kPrimaryWorkArea(0, 24, 1000, 776);
constexpr QRect kSecondaryWorkArea(1000, 24, 800, 576);

} // namespace

class MultiDisplayShellSurfaceTest : public ShellSurfaceTest
{
protected:
    display::DisplayManager displays;

    void SetUp() override
    {
        ShellSurfaceTest::SetUp();
        ASSERT_TRUE(displays.installHardware(
                {QStringLiteral("instance"),
                 QStringLiteral("seat0"),
                 1,
                 {physicalDisplay("a", {1000, 800}), physicalDisplay("b", {800, 600})}}));
        ASSERT_TRUE(displays.installApplied(
                {QStringLiteral("instance"),
                 1,
                 1,
                 {},
                 {logicalDisplay("a", kPrimary, true), logicalDisplay("b", kSecondary, false)},
                 {{displayId("b"), displayId("a"), display::LayoutEdge::Right, 0}}}));
        manager.setDisplayManager(&displays);
    }

    void TearDown() override
    {
        ShellSurfaceTest::TearDown();
        manager.setDisplayManager(nullptr);
    }

    void pointerAt(const QPointF &location) { seat.onPointerMotion(input::Timestamp(), location); }

    ShellSurface *mapApp(const QString &applicationId, const QSize &size,
                         Recorder **recorderOut = nullptr)
    {
        Recorder *recorder = nullptr;
        ShellSurface *shell = makeShell(&recorder);
        shell->setToplevel();
        shell->setApplicationId(applicationId);
        shell->rootSurface()->commit();
        EXPECT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
        attach(shell->rootSurface(), size);
        shell->rootSurface()->commit();
        compositor.updateTextures(renderer);
        if (recorderOut)
            *recorderOut = recorder;
        return shell;
    }
};

TEST_F(MultiDisplayShellSurfaceTest, TheActiveApplicationWindowDecidesTheOutput)
{
    pointerAt(QPointF(100, 100));
    ShellSurface *files = mapApp(QStringLiteral("files"), QSize(300, 200));
    files->window()->setBounds(QRect(1100, 100, 300, 200));
    files->window()->maximize();
    ASSERT_TRUE(files->window()->isActive());
    ASSERT_EQ(kSecondaryWorkArea, files->window()->bounds());

    // Inherited state and initial size come from the same output as placement.
    Recorder *recorder = nullptr;
    ShellSurface *shell = makeShell(&recorder);
    shell->setToplevel();
    shell->setApplicationId(QStringLiteral("files"));
    shell->rootSurface()->commit();
    ASSERT_EQ(1u, recorder->configures.size());
    EXPECT_EQ(WindowStateType::Maximized, recorder->configures.back().stateType);
    EXPECT_EQ(kSecondaryWorkArea.size(), recorder->configures.back().size);
    ASSERT_TRUE(shell->acknowledgeConfigure(recorder->serials.back()));
    attach(shell->rootSurface(), kSecondaryWorkArea.size());
    shell->rootSurface()->commit();
    EXPECT_EQ(kSecondaryWorkArea, shell->window()->bounds());

    ShellSurface *terminal = mapApp(QStringLiteral("terminal"), QSize(200, 100));
    EXPECT_TRUE(kSecondaryWorkArea.contains(terminal->window()->bounds()));
    EXPECT_EQ(kSecondaryWorkArea.center(), terminal->window()->bounds().center());
}

TEST_F(MultiDisplayShellSurfaceTest, WithoutAnActiveWindowThePointerThenThePrimaryDecides)
{
    pointerAt(QPointF(1400, 300));
    ShellSurface *first = mapApp(QStringLiteral("files"), QSize(200, 100));
    EXPECT_EQ(QRect(1300, 262, 200, 100), first->window()->bounds());

    manager.activation()->activate(nullptr);
    pointerAt(QPointF(-500, -500));
    ShellSurface *second = mapApp(QStringLiteral("terminal"), QSize(200, 100));
    EXPECT_EQ(QRect(400, 362, 200, 100), second->window()->bounds());
}

TEST_F(MultiDisplayShellSurfaceTest, TheCascadeStaysOnTheChosenOutput)
{
    pointerAt(QPointF(100, 100));
    ShellSurface *onPrimary = mapApp(QStringLiteral("files"), QSize(200, 100));
    ShellSurface *onSecondary = mapApp(QStringLiteral("files"), QSize(200, 100));
    onSecondary->window()->setBounds(QRect(1100, 100, 200, 100));
    ASSERT_TRUE(onSecondary->window()->isActive());

    ShellSurface *next = mapApp(QStringLiteral("files"), QSize(200, 100));
    EXPECT_EQ(QRect(1132, 132, 200, 100), next->window()->bounds());
    EXPECT_NE(onPrimary->window()->bounds().topLeft() + QPoint(32, 32),
              next->window()->bounds().topLeft());
}

TEST_F(MultiDisplayShellSurfaceTest, ADialogFollowsItsParentsOutput)
{
    pointerAt(QPointF(100, 100));
    ShellSurface *parent = mapApp(QStringLiteral("files"), QSize(400, 300));
    parent->window()->setBounds(QRect(1200, 100, 400, 300));
    manager.activation()->activate(nullptr);
    ShellSurface *other = mapApp(QStringLiteral("terminal"), QSize(200, 100));
    ASSERT_TRUE(other->window()->isActive());
    ASSERT_TRUE(kPrimary.contains(other->window()->bounds()));

    Recorder *recorder = nullptr;
    ShellSurface *dialog = makeShell(&recorder);
    dialog->setToplevel();
    dialog->setApplicationId(QStringLiteral("files"));
    ASSERT_TRUE(dialog->setTransientParent(parent));
    dialog->rootSurface()->commit();
    ASSERT_TRUE(dialog->acknowledgeConfigure(recorder->serials.back()));
    attach(dialog->rootSurface(), QSize(100, 80));
    dialog->rootSurface()->commit();
    EXPECT_EQ(QRect(1350, 210, 100, 80), dialog->window()->bounds());
}
