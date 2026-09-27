// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/stat.h>

#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include "clipboard_test_util.h"
#include "compositor/output.h"
#include "input/input_manager.h"
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

class FractionalScaleToolkit : public testing::TestWithParam<bool>
{
};

TEST_P(FractionalScaleToolkit, QtWindowDensityBuffersViewportAndPointer)
{
    QTemporaryDir runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().toLocal8Bit().constData(), 0700);

    struct RestoreRuntime {
        QByteArray previous = qgetenv("XDG_RUNTIME_DIR");

        ~RestoreRuntime() { qputenv("XDG_RUNTIME_DIR", previous); }
    } restore;

    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    Harness h;
    compositor::Output output;
    output.pixelSize = {1920, 1080};
    output.scale = *compositor::OutputScale::parse("1.5");
    h.compositor.setOutputScale(output.scale);
    h.scene.setSize(output.logicalSize());
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(QRect(QPoint(), output.logicalSize()));
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
    ASSERT_TRUE(h.server.addSocket(QStringLiteral("scale-toolkit")));
    ASSERT_TRUE(h.server.start());

    struct Child : QProcess {
        ~Child()
        {
            if (state() != NotRunning) {
                kill();
                waitForFinished(2000);
            }
        }
    } client;

    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto *name : {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                             "QT_QPA_PLATFORMTHEME", "QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS",
                             "QT_SCALE_FACTOR_ROUNDING_POLICY", "LD_PRELOAD"})
        env.remove(QString::fromLatin1(name));
    env.insert("QT_QPA_PLATFORM", "wayland");
    env.insert("WAYLAND_DISPLAY", h.server.socketName());
    env.insert("XDG_RUNTIME_DIR", runtime.path());
    env.insert("WAYLAND_DEBUG", "1");
    env.insert("QT_QUICK_BACKEND", "software");
    client.setProcessEnvironment(env);
    client.start(GetParam() ? QStringLiteral(QT_QUICK_SCALE_CLIENT)
                            : QStringLiteral(QT_SCALE_CLIENT));
    QByteArray log, wire;
    uint64_t generation = 0;
    auto pump = [&] {
        h.compositor.frameSubmitted(++generation);
        h.compositor.frameDrawn(wayland::nowInMilliseconds(), generation);
        log += client.readAllStandardOutput();
        wire += client.readAllStandardError();
        wayland::updateOutputMembership(outputGlobal);
    };
    ASSERT_TRUE(h.until(
            [&] {
                pump();
                return log.contains("windowDpr=1.50") && !wm.windows().empty()
                        && !h.compositor.surfaces().empty()
                        && h.compositor.surfaces().front()->bufferSize() == QSize(1202, 902);
            },
            7000))
            << log.constData() << wire.constData();
    auto *surface = h.compositor.surfaces().front();
    EXPECT_EQ(surface->contentSize(), QSize(801, 601));
    EXPECT_EQ(surface->viewport(), std::optional<QSize>({801, 601}));
    EXPECT_TRUE(log.contains("Qt=6.8.2")) << log.constData();
    EXPECT_TRUE(log.contains("screen=1280x720")) << log.constData();
    EXPECT_TRUE(wire.contains("preferred_scale(180)")) << wire.constData();
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
                return log.contains("POINTER 201.00 150.00");
            },
            3000))
            << log.constData() << wire.constData();
    EXPECT_TRUE(log.contains("ENTER 200.00 150.00")) << log.constData();
    QObject::connect(&h.compositor, &compositor::Compositor::outputScaleChanged, &wm, [&] {
        output.scale = h.compositor.outputScale();
        wm.setOutputBounds(QRect(QPoint(), output.logicalSize()));
        h.scene.setSize(output.logicalSize());
        input.setDeviceScale(output.scale.value());
        input.setBounds(output.logicalSize());
        wayland::updateOutputGlobal(outputGlobal, &output);
    });
    for (const auto *value : {"1.25", "1.75", "2", "1"}) {
        const auto previousViewport = surface->viewport();
        h.compositor.setOutputScale(*compositor::OutputScale::parse(QString::fromLatin1(value)));
        EXPECT_EQ(surface->viewport(), previousViewport);
        const QSize logical = window->bounds().size();
        const QSize expected(qRound(logical.width() * output.scale.value()),
                             qRound(logical.height() * output.scale.value()));
        ASSERT_TRUE(h.until(
                [&] {
                    pump();
                    return surface->bufferSize() == expected
                            && log.contains(QStringLiteral("screen=%1x%2")
                                                    .arg(output.logicalSize().width())
                                                    .arg(output.logicalSize().height())
                                                    .toUtf8());
                },
                5000))
                << log.constData() << wire.constData();
        EXPECT_EQ(surface->contentSize(), logical);
        EXPECT_EQ(surface->viewport(), std::optional<QSize>(logical));
    }
    std::printf("%s", log.constData());
}

INSTANTIATE_TEST_SUITE_P(WidgetsAndQuick, FractionalScaleToolkit, testing::Bool());

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
