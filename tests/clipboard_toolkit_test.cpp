// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdlib>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "clipboard_test_util.h"
#include "compositor/output.h"
#include "process_test_util.h"
#include "temporary_directory.h"
#include "wayland/protocols/data_control.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wl_shm.h"
#include "wayland/protocols/wl_subcompositor.h"
#include "wayland/protocols/xdg_decoration.h"
#include "wayland/protocols/xdg_shell.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace clipboard_test;

namespace {

// A write to a client that went away fails instead of ending the test.
const bool sigpipeIgnored = [] {
    signal(SIGPIPE, SIG_IGN);
    return true;
}();

using Child = test::ChildProcess;

std::string helperProgram()
{
    const char *helper = getenv("CUTEFISH_CLIPBOARD_TEST_HELPER");
    return helper ? helper : "";
}

void matrix(bool sourceGtk, bool destinationGtk, bool binary = false, bool persist = false,
            bool urls = false)
{
    test::TemporaryDirectory runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().c_str(), 0700);

    struct RestoreRuntime {
        const char *value = getenv("XDG_RUNTIME_DIR");
        std::string previous = value ? value : "";

        ~RestoreRuntime() { setenv("XDG_RUNTIME_DIR", previous.c_str(), 1); }
    } restoreRuntime;

    setenv("XDG_RUNTIME_DIR", runtime.path().c_str(), 1);
    Harness h;
    std::unique_ptr<wayland::DataControlManager> dataControl;
    if (persist)
        dataControl = std::make_unique<wayland::DataControlManager>(
                h.server.display(), &h.waylandSeat, [](const wl_client *) {
                    return true;
                });
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(base::Rect(0, 0, 800, 600));
    wayland::WaylandXdgShell xdg(&h.compositor, &wm, &h.seat, h.server.serialTracker());

    struct DestroyClients {
        wl_display *display;

        ~DestroyClients() { wl_display_destroy_clients(display); }
    } cleanup {h.server.display()};

    compositor::Output output;
    output.pixelSize = base::Size(800, 600);
    wayland::createSubcompositorGlobal(h.server.display());
    wayland::createShmGlobal(h.server.display(), &h.compositor);
    wayland::createOutputGlobal(h.server.display(), &output);
    wayland::createXdgShellGlobal(h.server.display(), &xdg);
    wayland::createXdgDecorationGlobal(h.server.display());
    ASSERT_TRUE(h.server.addSocket("clipboard-test"));
    ASSERT_TRUE(h.server.start());
    Child source, destination, service;
    auto start = [&](Child &process, bool gtk, const std::string &title) {
        const auto environment = test::environmentWith(
                {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORMTHEME",
                 "QT_QUICK_BACKEND", "LD_PRELOAD"},
                {{"QT_QPA_PLATFORM", "wayland"},
                 {"GDK_BACKEND", "wayland"},
                 {"WAYLAND_DISPLAY", h.server.socketName()},
                 {"XDG_RUNTIME_DIR", runtime.path()}});
        const std::string mode = urls ? "urls" : binary ? "binary" : "text";
        process.start(gtk ? "/usr/bin/python3" : QT_CLIPBOARD_CLIENT,
                      gtk ? std::vector<std::string> {GTK_CLIPBOARD_CLIENT, title}
                          : std::vector<std::string> {title, mode},
                      environment);
    };
    auto window = [&](const std::string &title) -> wm::Window * {
        for (const auto &w : wm.windows())
            if (w->title() == title)
                return w;
        return nullptr;
    };
    start(source, sourceGtk, "clipboard-source");
    ASSERT_TRUE(h.until(
            [&] {
                return window("clipboard-source") != nullptr;
            },
            5000))
            << source.errors();
    window("clipboard-source")->activate();
    h.seat.onKey(input::Timestamp {}, urls ? 50 : 46, true);
    h.seat.onKey(input::Timestamp {}, urls ? 50 : 46, false);
    if (urls) {
        ASSERT_TRUE(h.until(
                [&] {
                    return source.saw("MENU");
                },
                5000));
        for (int i = 0; i < 20; ++i)
            h.pump();
        h.seat.onKey({}, 108, true);
        h.seat.onKey({}, 108, false);
        h.seat.onKey({}, 28, true);
        h.seat.onKey({}, 28, false);
    }
    ASSERT_TRUE(h.until(
            [&] {
                return source.saw("PUBLISHED") && h.seat.selection()->source();
            },
            5000))
            << source.errors();
    if (persist) {
        ASSERT_TRUE(service.start(helperProgram(), {}, source.environment));
        ASSERT_TRUE(h.until(
                [&] {
                    auto selection = h.seat.selection()->source();
                    return selection && selection->supports("application/x-cutefish-clipboard");
                },
                7000))
                << service.errors();
        window("clipboard-source")->activate();
        if (urls) {
            h.seat.onKey({}, 25, true);
            h.seat.onKey({}, 25, false);
            ASSERT_TRUE(h.until(
                    [&] {
                        return source.saw("PASS") || !source.running();
                    },
                    5000));
            ASSERT_TRUE(source.saw("PASS")) << source.output();
        }
        h.seat.onKey({}, 16, true);
        h.seat.onKey({}, 16, false);
        ASSERT_TRUE(h.until([&] {
            return !source.running();
        }));
        EXPECT_EQ(0, source.exitCode);
    }
    start(destination, destinationGtk, "clipboard-destination");
    ASSERT_TRUE(h.until(
            [&] {
                return window("clipboard-destination") != nullptr;
            },
            5000))
            << destination.errors();
    window("clipboard-destination")->activate();
    h.seat.onKey(input::Timestamp {}, 47, true);
    h.seat.onKey(input::Timestamp {}, 47, false);
    ASSERT_TRUE(h.until(
            [&] {
                return destination.saw("PASS") || !destination.running();
            },
            5000));
    EXPECT_TRUE(destination.saw("PASS"))
            << destination.output() << destination.errors();
    ASSERT_TRUE(h.until(
            [&] {
                return !destination.running();
            },
            5000));
    EXPECT_FALSE(destination.crashed);
    EXPECT_EQ(0, destination.exitCode) << destination.errors();
    if (!persist) {
        ASSERT_TRUE(window("clipboard-source"));
        window("clipboard-source")->activate();
        h.seat.onKey(input::Timestamp {}, 16, true);
        h.seat.onKey(input::Timestamp {}, 16, false);
        ASSERT_TRUE(h.until(
                [&] {
                    return !source.running();
                },
                5000));
        EXPECT_FALSE(source.crashed);
        EXPECT_EQ(0, source.exitCode) << source.errors();
    }
    wl_display_destroy_clients(h.server.display());
}

} // namespace

TEST(ClipboardToolkit, QtToGtk)
{
    matrix(false, true);
}

TEST(ClipboardToolkit, GtkToQt)
{
    matrix(true, false);
}

TEST(ClipboardToolkit, QtToQt)
{
    matrix(false, false);
}

TEST(ClipboardToolkit, QtRawBinaryExactBytes)
{
    matrix(false, false, true);
}

TEST(ClipboardToolkit, ServicePersistsQtAndGtkCopiesAfterTheirWindowsExit)
{
    if (helperProgram().empty())
        GTEST_SKIP() << "Set CUTEFISH_CLIPBOARD_TEST_HELPER to the standalone clipboard helper";
    matrix(false, true, false, true);
    matrix(true, false, false, true);
    matrix(false, false, true, true);
}

TEST(ClipboardToolkit, ServicePreservesFileUrlsAfterContextMenuCopy)
{
    if (helperProgram().empty())
        GTEST_SKIP() << "Set CUTEFISH_CLIPBOARD_TEST_HELPER to the standalone clipboard helper";
    matrix(false, false, false, true, true);
}
