// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <linux/input-event-codes.h>
#include <signal.h>
#include <sys/stat.h>

#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#include "clipboard_test_util.h"
#include "compositor/output.h"
#include "input/input_manager.h"
#include "scene/surface_node.h"
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

class Child : public QProcess
{
public:
    QByteArray output;

    ~Child() override
    {
        if (state() != NotRunning) {
            kill();
            waitForFinished(2000);
        }
    }

    bool saw(const char* marker)
    {
        output += readAllStandardOutput();
        return output.contains(marker);
    }
};

void matrix(bool sourceGtk, bool destinationGtk)
{
    QTemporaryDir runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().toLocal8Bit().constData(), 0700);

    struct RestoreRuntime {
        QByteArray previous = qgetenv("XDG_RUNTIME_DIR");

        ~RestoreRuntime() { qputenv("XDG_RUNTIME_DIR", previous); }
    } restoreRuntime;

    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    Harness h;
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(QRect(0, 0, 800, 600));
    wayland::WaylandXdgShell xdg(&h.compositor, &wm, &h.seat, h.server.serialTracker());

    struct DestroyClients {
        wl_display* display;

        ~DestroyClients() { wl_display_destroy_clients(display); }
    } cleanup {h.server.display()};

    compositor::Output output;
    output.pixelSize = QSize(800, 600);
    wayland::createSubcompositorGlobal(h.server.display());
    wayland::createShmGlobal(h.server.display(), &h.compositor);
    wayland::createOutputGlobal(h.server.display(), &output);
    wayland::createXdgShellGlobal(h.server.display(), &xdg);
    wayland::createXdgDecorationGlobal(h.server.display());
    ASSERT_TRUE(h.server.addSocket(QStringLiteral("dnd-test")));
    ASSERT_TRUE(h.server.start());
    QTimer frames;
    QObject::connect(&frames, &QTimer::timeout, &frames, [&] {
        h.compositor.frameSubmitted();
        h.compositor.frameDrawn(wayland::nowInMilliseconds());
    });
    frames.start(5);
    Child source, destination;
    auto start = [&](Child& process, bool gtk, const QString& title) {
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto* name : {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                 "QT_QPA_PLATFORMTHEME", "QT_QUICK_BACKEND", "LD_PRELOAD"})
            env.remove(QString::fromLatin1(name));
        env.insert("QT_QPA_PLATFORM", "wayland");
        env.insert("GDK_BACKEND", "wayland");
        env.insert("WAYLAND_DISPLAY", h.server.socketName());
        env.insert("XDG_RUNTIME_DIR", runtime.path());
        process.setProcessEnvironment(env);
        process.start(gtk ? QStringLiteral("/usr/bin/python3") : QStringLiteral(QT_DND_CLIENT),
                      gtk ? QStringList {QStringLiteral(GTK_DND_CLIENT), title}
                          : QStringList {title});
    };
    auto window = [&](const QString& title) -> wm::Window* {
        for (const auto& w : wm.windows())
            if (w->title() == title)
                return w;
        return nullptr;
    };
    start(source, sourceGtk, QStringLiteral("dnd-source"));
    ASSERT_TRUE(h.until(
        [&] {
            return window("dnd-source") != nullptr;
        },
        5000))
        << source.readAllStandardError().constData();
    start(destination, destinationGtk, QStringLiteral("dnd-destination"));
    ASSERT_TRUE(h.until(
        [&] {
            return window("dnd-destination") != nullptr;
        },
        5000))
        << destination.readAllStandardError().constData();
    auto* sourceWindow = window("dnd-source");
    auto* destinationWindow = window("dnd-destination");
    sourceWindow->setBounds(QRect(QPoint(20, 20), sourceWindow->bounds().size()));
    destinationWindow->setBounds(QRect(QPoint(360, 20), destinationWindow->bounds().size()));
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
    const QPointF startPosition = sourceWindow->surfaceNode()->absolutePosition() + QPoint(60, 85);
    const QPointF endPosition =
        destinationWindow->surfaceNode()->absolutePosition() + QPoint(60, 85);
    input.onPointerMotionAbsolute({}, startPosition);
    input.onPointerButton({}, BTN_LEFT, true);
    for (int i = 0; i < 5; ++i)
        h.pump();
    input.onPointerMotionAbsolute({}, startPosition + QPoint(25, 15));
    ASSERT_TRUE(h.until(
        [&] {
            return h.seat.drag()->active();
        },
        5000))
        << source.readAllStandardError().constData();
    input.onPointerMotionAbsolute({}, endPosition);
    for (int i = 0; i < 30; ++i)
        h.pump();
    input.onPointerButton({}, BTN_LEFT, false);
    EXPECT_TRUE(h.until(
        [&] {
            return destination.saw("PASS");
        },
        5000))
        << destination.output.constData() << destination.readAllStandardError().constData();
    EXPECT_TRUE(h.until(
        [&] {
            return source.saw("FINISHED");
        },
        5000))
        << source.output.constData() << source.readAllStandardError().constData();
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_EQ(input::NoButton, h.seat.pressedButtons());
    wl_display_destroy_clients(h.server.display());
}

// Qt 6 attaches a window to a drag through xdg_toplevel_drag_v1 when the mime
// data names it, which is how an application pulls a tab out into a window.
TEST(DndToolkit, QtCarriesADetachedWindowWithTheDrag)
{
    QTemporaryDir runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().toLocal8Bit().constData(), 0700);

    struct RestoreRuntime {
        QByteArray previous = qgetenv("XDG_RUNTIME_DIR");

        ~RestoreRuntime() { qputenv("XDG_RUNTIME_DIR", previous); }
    } restoreRuntime;

    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    Harness h;
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(QRect(0, 0, 800, 600));
    wayland::WaylandXdgShell xdg(&h.compositor, &wm, &h.seat, h.server.serialTracker());

    struct DestroyClients {
        wl_display* display;

        ~DestroyClients() { wl_display_destroy_clients(display); }
    } cleanup {h.server.display()};

    compositor::Output output;
    output.pixelSize = QSize(800, 600);
    wayland::createSubcompositorGlobal(h.server.display());
    wayland::createShmGlobal(h.server.display(), &h.compositor);
    wayland::createOutputGlobal(h.server.display(), &output);
    wayland::createXdgShellGlobal(h.server.display(), &xdg);
    wayland::createXdgDecorationGlobal(h.server.display());
    wayland::createXdgToplevelDragGlobal(h.server.display(), &wm);
    ASSERT_TRUE(h.server.addSocket(QStringLiteral("dnd-test")));
    ASSERT_TRUE(h.server.start());
    QTimer frames;
    QObject::connect(&frames, &QTimer::timeout, &frames, [&] {
        h.compositor.frameSubmitted();
        h.compositor.frameDrawn(wayland::nowInMilliseconds());
    });
    frames.start(5);

    Child client;
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto* name : {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                             "QT_QPA_PLATFORMTHEME", "QT_QUICK_BACKEND", "LD_PRELOAD"})
        env.remove(QString::fromLatin1(name));
    env.insert("QT_QPA_PLATFORM", "wayland");
    env.insert("WAYLAND_DISPLAY", h.server.socketName());
    env.insert("XDG_RUNTIME_DIR", runtime.path());
    client.setProcessEnvironment(env);
    client.start(QStringLiteral(QT_DND_CLIENT), {QStringLiteral("tab-source")});

    auto window = [&](const QString& title) -> wm::Window* {
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
        << client.readAllStandardError().constData();
    auto* sourceWindow = window("tab-source");
    sourceWindow->setBounds(QRect(QPoint(20, 20), sourceWindow->bounds().size()));
    ASSERT_TRUE(h.until([&] {
        return sourceWindow->rootSurface()->isMapped()
            && sourceWindow->surfaceNode()->absolutePosition()
            == sourceWindow->contentBounds().topLeft() + sourceWindow->surfaceOrigin();
    }));
    sourceWindow->activate();

    input::InputManager input(&h.seat);
    input.setEventSink(&wm);
    input.setBounds({800, 600});
    const QPointF startPosition = sourceWindow->surfaceNode()->absolutePosition() + QPoint(60, 85);
    input.onPointerMotionAbsolute({}, startPosition);
    input.onPointerButton({}, BTN_LEFT, true);
    for (int i = 0; i < 5; ++i)
        h.pump();
    const QPointF grabbed = startPosition + QPoint(25, 15);
    input.onPointerMotionAbsolute({}, grabbed);
    ASSERT_TRUE(h.until(
        [&] {
            return h.seat.drag()->active() && window("tab-detached") != nullptr;
        },
        5000))
        << client.readAllStandardError().constData();

    // It maps with the pointer at its offset, and moves with the pointer.
    wm::Window* detached = window("tab-detached");
    EXPECT_EQ(grabbed.toPoint() - QPoint(10, 10), detached->contentBounds().topLeft());
    input.onPointerMotionAbsolute({}, QPointF(500, 300));
    for (int i = 0; i < 5; ++i)
        h.pump();
    EXPECT_EQ(QPoint(490, 290), detached->contentBounds().topLeft());

    // Let go over nothing that takes it: a drop all the same, and the window stays.
    input.onPointerButton({}, BTN_LEFT, false);
    EXPECT_TRUE(h.until(
        [&] {
            return client.saw("DROPPED");
        },
        5000))
        << client.output.constData() << client.readAllStandardError().constData();
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_FALSE(wm.eventHandler()->isDragging());
    ASSERT_EQ(detached, window("tab-detached"));
    EXPECT_EQ(QPoint(490, 290), detached->contentBounds().topLeft());
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
