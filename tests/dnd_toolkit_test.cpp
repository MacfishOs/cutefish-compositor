// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <linux/input-event-codes.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "base/geometry.h"
#include "base/process.h"
#include "base/timer.h"
#include "clipboard_test_util.h"
#include "compositor/output.h"
#include "input/input_manager.h"
#include "process_test_util.h"
#include "scene/surface_node.h"
#include "temporary_directory.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wl_shm.h"
#include "wayland/protocols/wl_subcompositor.h"
#include "wayland/protocols/xdg_decoration.h"
#include "wayland/protocols/xdg_shell.h"
#include "wayland/protocols/xdg_toplevel_drag.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace clipboard_test;

namespace {

// Killed and waited for when it goes.
class Child : public base::Process
{
public:
    bool saw(const char* marker) const { return test::contains(standardOutput(), marker); }
};

void matrix(bool sourceGtk, bool destinationGtk)
{
    test::TemporaryDirectory runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().c_str(), 0700);

    const test::ScopedEnvironmentVariable runtimeVariable("XDG_RUNTIME_DIR", runtime.path());
    Harness h;
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(base::Rect(0, 0, 800, 600));
    wayland::WaylandXdgShell xdg(&h.compositor, &wm, &h.seat, h.server.serialTracker());

    struct DestroyClients {
        wl_display* display;

        ~DestroyClients() { wl_display_destroy_clients(display); }
    } cleanup {h.server.display()};

    compositor::Output output;
    output.pixelSize = base::Size(800, 600);
    wayland::createSubcompositorGlobal(h.server.display());
    wayland::createShmGlobal(h.server.display(), &h.compositor);
    wayland::createOutputGlobal(h.server.display(), &output);
    wayland::createXdgShellGlobal(h.server.display(), &xdg);
    wayland::createXdgDecorationGlobal(h.server.display());
    ASSERT_TRUE(h.server.addSocket("dnd-test"));
    ASSERT_TRUE(h.server.start());
    base::Timer frames;
    frames.onTimeout = [&] {
        h.compositor.frameSubmitted();
        h.compositor.frameDrawn(wayland::nowInMilliseconds());
    };
    frames.start(std::chrono::milliseconds(5));
    Child source, destination;
    auto start = [&](Child& process, bool gtk, const std::string& title) {
        process.setEnvironment(test::environmentWith(
            {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORMTHEME",
             "QT_QUICK_BACKEND", "LD_PRELOAD"},
            {{"QT_QPA_PLATFORM", "wayland"},
             {"GDK_BACKEND", "wayland"},
             {"WAYLAND_DISPLAY", h.server.socketName()},
             {"XDG_RUNTIME_DIR", runtime.path()}}));
        process.start(gtk ? "/usr/bin/python3" : QT_DND_CLIENT,
                      gtk ? std::vector<std::string> {GTK_DND_CLIENT, title}
                          : std::vector<std::string> {title});
    };
    auto window = [&](const std::string& title) -> wm::Window* {
        for (const auto& w : wm.windows())
            if (w->title() == title)
                return w;
        return nullptr;
    };
    start(source, sourceGtk, "dnd-source");
    ASSERT_TRUE(h.until(
        [&] {
            return window("dnd-source") != nullptr;
        },
        5000))
        << source.standardError();
    start(destination, destinationGtk, "dnd-destination");
    ASSERT_TRUE(h.until(
        [&] {
            return window("dnd-destination") != nullptr;
        },
        5000))
        << destination.standardError();
    auto* sourceWindow = window("dnd-source");
    auto* destinationWindow = window("dnd-destination");
    sourceWindow->setBounds(base::Rect(base::Point(20, 20), sourceWindow->bounds().size()));
    destinationWindow->setBounds(base::Rect(base::Point(360, 20), destinationWindow->bounds().size()));
    ASSERT_TRUE(h.until([&] {
        return sourceWindow->rootSurface()->isMapped() &&
            destinationWindow->rootSurface()->isMapped() &&
            sourceWindow->surfaceNode()->absolutePosition() ==
            sourceWindow->contentBounds().topLeft() + sourceWindow->surfaceOrigin() &&
            destinationWindow->surfaceNode()->absolutePosition() ==
            destinationWindow->contentBounds().topLeft() + destinationWindow->surfaceOrigin();
    }));
    sourceWindow->activate();
    input::InputManager input(&h.seat);
    input.setEventSink(&wm);
    input.setBounds({800, 600});
    const base::PointF startPosition = sourceWindow->surfaceNode()->absolutePosition() + base::Point(60, 85);
    const base::PointF endPosition =
        destinationWindow->surfaceNode()->absolutePosition() + base::Point(60, 85);
    input.onPointerMotionAbsolute({}, startPosition);
    input.onPointerButton({}, BTN_LEFT, true);
    for (int i = 0; i < 5; ++i)
        h.pump();
    input.onPointerMotionAbsolute({}, startPosition + base::PointF(25, 15));
    ASSERT_TRUE(h.until(
        [&] {
            return h.seat.drag()->active();
        },
        5000))
        << source.standardError();
    input.onPointerMotionAbsolute({}, endPosition);
    // A release before the destination has answered the enter with an accepted
    // type and action is a cancel, however long a busy machine takes to answer.
    ASSERT_TRUE(h.until(
        [&] {
            return h.seat.drag()->targetAcceptsDrop();
        },
        5000))
        << destination.standardOutput() << destination.standardError();
    input.onPointerButton({}, BTN_LEFT, false);
    EXPECT_TRUE(h.until(
        [&] {
            return destination.saw("PASS");
        },
        5000))
        << destination.standardOutput() << destination.standardError();
    EXPECT_TRUE(h.until(
        [&] {
            return source.saw("FINISHED");
        },
        5000))
        << source.standardOutput() << source.standardError();
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_EQ(input::NoButton, h.seat.pressedButtons());
    wl_display_destroy_clients(h.server.display());
}

// Qt 6 attaches a window to a drag through xdg_toplevel_drag_v1 when the mime
// data names it, which is how an application pulls a tab out into a window.
TEST(DndToolkit, QtCarriesADetachedWindowWithTheDrag)
{
    test::TemporaryDirectory runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().c_str(), 0700);

    const test::ScopedEnvironmentVariable runtimeVariable("XDG_RUNTIME_DIR", runtime.path());
    Harness h;
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(base::Rect(0, 0, 800, 600));
    wayland::WaylandXdgShell xdg(&h.compositor, &wm, &h.seat, h.server.serialTracker());

    struct DestroyClients {
        wl_display* display;

        ~DestroyClients() { wl_display_destroy_clients(display); }
    } cleanup {h.server.display()};

    compositor::Output output;
    output.pixelSize = base::Size(800, 600);
    wayland::createSubcompositorGlobal(h.server.display());
    wayland::createShmGlobal(h.server.display(), &h.compositor);
    wayland::createOutputGlobal(h.server.display(), &output);
    wayland::createXdgShellGlobal(h.server.display(), &xdg);
    wayland::createXdgDecorationGlobal(h.server.display());
    wayland::createXdgToplevelDragGlobal(h.server.display(), &wm);
    ASSERT_TRUE(h.server.addSocket("dnd-test"));
    ASSERT_TRUE(h.server.start());
    base::Timer frames;
    frames.onTimeout = [&] {
        h.compositor.frameSubmitted();
        h.compositor.frameDrawn(wayland::nowInMilliseconds());
    };
    frames.start(std::chrono::milliseconds(5));

    Child client;
    client.setEnvironment(test::environmentWith(
        {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORMTHEME",
         "QT_QUICK_BACKEND", "LD_PRELOAD"},
        {{"QT_QPA_PLATFORM", "wayland"},
         {"WAYLAND_DISPLAY", h.server.socketName()},
         {"XDG_RUNTIME_DIR", runtime.path()}}));
    client.start(QT_DND_CLIENT, {"tab-source"});

    auto window = [&](const std::string& title) -> wm::Window* {
        for (const auto& w : wm.windows())
            if (w->title() == title)
                return w;
        return nullptr;
    };
    ASSERT_TRUE(h.until(
        [&] {
            return window("tab-source") != nullptr;
        },
        5000))
        << client.standardError();
    auto* sourceWindow = window("tab-source");
    sourceWindow->setBounds(base::Rect(base::Point(20, 20), sourceWindow->bounds().size()));
    ASSERT_TRUE(h.until([&] {
        return sourceWindow->rootSurface()->isMapped()
            && sourceWindow->surfaceNode()->absolutePosition()
            == sourceWindow->contentBounds().topLeft() + sourceWindow->surfaceOrigin();
    }));
    sourceWindow->activate();

    input::InputManager input(&h.seat);
    input.setEventSink(&wm);
    input.setBounds({800, 600});
    const base::PointF startPosition = sourceWindow->surfaceNode()->absolutePosition() + base::Point(60, 85);
    input.onPointerMotionAbsolute({}, startPosition);
    input.onPointerButton({}, BTN_LEFT, true);
    for (int i = 0; i < 5; ++i)
        h.pump();
    const base::PointF grabbed = startPosition + base::PointF(25, 15);
    input.onPointerMotionAbsolute({}, grabbed);
    ASSERT_TRUE(h.until(
        [&] {
            return h.seat.drag()->active() && window("tab-detached") != nullptr;
        },
        5000))
        << client.standardError();

    // It maps with the pointer at its offset, and moves with the pointer.
    wm::Window* detached = window("tab-detached");
    EXPECT_EQ(grabbed.toPoint() - base::Point(10, 10), detached->contentBounds().topLeft());
    input.onPointerMotionAbsolute({}, base::PointF(500, 300));
    for (int i = 0; i < 5; ++i)
        h.pump();
    EXPECT_EQ(base::Point(490, 290), detached->contentBounds().topLeft());

    // Let go over nothing that takes it: a drop all the same, and the window stays.
    input.onPointerButton({}, BTN_LEFT, false);
    EXPECT_TRUE(h.until(
        [&] {
            return client.saw("DROPPED");
        },
        5000))
        << client.standardOutput() << client.standardError();
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_FALSE(wm.eventHandler()->isDragging());
    ASSERT_EQ(detached, window("tab-detached"));
    EXPECT_EQ(base::Point(490, 290), detached->contentBounds().topLeft());
    wl_display_destroy_clients(h.server.display());
}

} // namespace

TEST(DndToolkit, QtToGtk)
{
    matrix(false, true);
}

TEST(DndToolkit, GtkToQt)
{
    matrix(true, false);
}

TEST(DndToolkit, QtToQt)
{
    matrix(false, false);
}

TEST(DndToolkit, GtkToGtk)
{
    matrix(true, true);
}
