// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/stat.h>

#include <format>
#include <optional>
#include <string>

#include "base/process.h"
#include "clipboard_test_util.h"
#include "compositor/output.h"
#include "input/input_manager.h"
#include "process_test_util.h"
#include "temporary_directory.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wl_shm.h"
#include "wayland/protocols/wl_subcompositor.h"
#include "wayland/protocols/wp_fractional_scale.h"
#include "wayland/protocols/wp_viewporter.h"
#include "wayland/protocols/xdg_decoration.h"
#include "wayland/protocols/xdg_shell.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace clipboard_test;

using cutefish::test::contains;

class FractionalScaleToolkit : public testing::TestWithParam<bool>
{
};

TEST_P(FractionalScaleToolkit, QtWindowDensityBuffersViewportAndPointer)
{
    test::TemporaryDirectory runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().c_str(), 0700);

    const test::ScopedEnvironmentVariable runtimeVariable("XDG_RUNTIME_DIR", runtime.path());
    Harness h;
    compositor::Output output;
    output.pixelSize = {1920, 1080};
    output.scale = *compositor::OutputScale::parse("1.5");
    h.compositor.setOutputScale(output.scale);
    h.scene.setSize(output.logicalSize());
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(base::Rect(base::Point(), output.logicalSize()));
    wayland::WaylandXdgShell xdg(&h.compositor, &wm, &h.seat, h.server.serialTracker());

    struct Cleanup {
        wl_display *display;

        ~Cleanup() { wl_display_destroy_clients(display); }
    } cleanup {h.server.display()};

    wayland::createSubcompositorGlobal(h.server.display());
    wayland::createShmGlobal(h.server.display(), &h.compositor);
    auto *outputGlobal = wayland::createOutputGlobal(h.server.display(), &output);
    wayland::createXdgOutputManagerGlobal(h.server.display());
    wayland::createFractionalScaleGlobal(h.server.display());
    wayland::createViewporterGlobal(h.server.display());
    wayland::createXdgShellGlobal(h.server.display(), &xdg);
    wayland::createXdgDecorationGlobal(h.server.display());
    ASSERT_TRUE(h.server.addSocket("scale-toolkit"));
    ASSERT_TRUE(h.server.start());

    // Killed and waited for when it goes.
    base::Process client;
    client.setEnvironment(test::environmentWith(
            {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORMTHEME",
             "QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_SCALE_FACTOR_ROUNDING_POLICY",
             "LD_PRELOAD"},
            {{"QT_QPA_PLATFORM", "wayland"},
             {"WAYLAND_DISPLAY", h.server.socketName()},
             {"XDG_RUNTIME_DIR", runtime.path()},
             {"WAYLAND_DEBUG", "1"},
             {"QT_QUICK_BACKEND", "software"}}));
    client.start(GetParam() ? QT_QUICK_SCALE_CLIENT : QT_SCALE_CLIENT);
    std::string log, wire;
    uint64_t generation = 0;
    auto pump = [&] {
        h.compositor.frameSubmitted(++generation);
        h.compositor.frameDrawn(wayland::nowInMilliseconds(), generation);
        log = client.standardOutput();
        wire = client.standardError();
        wayland::updateOutputMembership(outputGlobal);
    };
    ASSERT_TRUE(h.until(
            [&] {
                pump();
                return contains(log, "windowDpr=1.50") && !wm.windows().empty()
                        && !h.compositor.surfaces().empty()
                        && h.compositor.surfaces().front()->bufferSize()
                               == base::Size(1202, 902);
            },
            7000))
            << log << wire;
    auto *surface = h.compositor.surfaces().front();
    EXPECT_EQ(surface->contentSize(), base::Size(801, 601));
    EXPECT_EQ(surface->viewport(), std::optional<base::Size>({801, 601}));
    EXPECT_TRUE(contains(log, "Qt=6.8.2")) << log;
    EXPECT_TRUE(contains(log, "screen=1280x720")) << log;
    EXPECT_TRUE(contains(wire, "preferred_scale(180)")) << wire;
    auto *window = wm.windows().front();
    window->setBounds({100, 50, 801, 601});
    input::InputManager input(&h.seat);
    input.setDeviceBounds(output.pixelSize);
    input.setDeviceScale(output.scale.value());
    input.setBounds(output.logicalSize());
    input.onPointerMotionAbsolute({}, {450, 300});
    input.onPointerMotion({}, {1.5, 0});
    ASSERT_TRUE(h.until(
            [&] {
                pump();
                return contains(log, "POINTER 201.00 150.00");
            },
            3000))
            << log << wire;
    EXPECT_TRUE(contains(log, "ENTER 200.00 150.00")) << log;
    const auto scaleChanged = h.compositor.outputScaleChanged.connect([&] {
        output.scale = h.compositor.outputScale();
        wm.setOutputBounds(base::Rect(base::Point(), output.logicalSize()));
        h.scene.setSize(output.logicalSize());
        input.setDeviceScale(output.scale.value());
        input.setBounds(output.logicalSize());
        wayland::updateOutputGlobal(outputGlobal, &output);
    });
    for (const auto *value : {"1.25", "1.75", "2", "1"}) {
        const auto previousViewport = surface->viewport();
        h.compositor.setOutputScale(*compositor::OutputScale::parse(value));
        EXPECT_EQ(surface->viewport(), previousViewport);
        const base::Size logical = window->bounds().size();
        const base::Size expected(base::roundToInt(logical.width() * output.scale.value()),
                                  base::roundToInt(logical.height() * output.scale.value()));
        ASSERT_TRUE(h.until(
                [&] {
                    pump();
                    return surface->bufferSize() == expected
                            && contains(log, std::format("screen={}x{}",
                                                         output.logicalSize().width(),
                                                         output.logicalSize().height()));
                },
                5000))
                << log << wire;
        EXPECT_EQ(surface->contentSize(), logical);
        EXPECT_EQ(surface->viewport(), std::optional<base::Size>(logical));
    }
    std::printf("%s", log.c_str());
}

INSTANTIATE_TEST_SUITE_P(WidgetsAndQuick, FractionalScaleToolkit, testing::Bool());
