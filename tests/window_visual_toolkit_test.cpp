// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QFileInfo>
#include <QImage>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include "clipboard_test_util.h"
#include "compositor/output.h"
#include "decoration/decoration.h"
#include "decoration/decoration_theme.h"
#include "decoration/shadow_node.h"
#include "egl_test_util.h"
#include "scene/surface_node.h"
#include "scene/window_animation.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wl_shm.h"
#include "wayland/protocols/wl_subcompositor.h"
#include "wayland/protocols/wp_fractional_scale.h"
#include "wayland/protocols/wp_viewporter.h"
#include "wayland/protocols/xdg_decoration.h"
#include "wayland/protocols/xdg_shell.h"
#include "wm/shadow_controller.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_visual_controller.h"
#include "wm/wm_shadow_controller_delegate.h"

using namespace cutefish;

namespace {

void acceptClient(const QString &program, QStringList arguments, scene::WindowVisualMode expected,
                  bool exerciseResize = false, bool interactiveResize = false,
                  bool eglResize = false, double bufferScale = 1.0, bool exerciseDamage = false)
{
    QTemporaryDir runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().toLocal8Bit().constData(), 0700);

    struct RestoreRuntime {
        QByteArray previous = qgetenv("XDG_RUNTIME_DIR");

        ~RestoreRuntime() { qputenv("XDG_RUNTIME_DIR", previous); }
    } restore;

    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());

    struct DeviceFd {
        int fd = -1;

        ~DeviceFd()
        {
            if (fd >= 0)
                close(fd);
        }
    } deviceFd;

    std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> device(nullptr, gbm_device_destroy);
    std::unique_ptr<gbm_surface, decltype(&gbm_surface_destroy)> gbmSurface(nullptr,
                                                                            gbm_surface_destroy);
    std::unique_ptr<render::EglRenderer> renderer;
    std::unique_ptr<render::EglRenderTarget> target;
    const auto devicePath = qgetenv("CUTEFISH_TEST_DRM_DEVICE");
    if (devicePath.isEmpty()) {
        renderer = render::EglRendererTestAccess::create();
        ASSERT_NE(renderer, nullptr);
        const EGLint attrs[] = {EGL_WIDTH, 1920, EGL_HEIGHT, 1080, EGL_NONE};
        const auto eglSurface =
                eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attrs);
        ASSERT_NE(eglSurface, EGL_NO_SURFACE);
        target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), eglSurface,
                                                           nullptr, QSize(1920, 1080));
    } else {
        deviceFd.fd = open(devicePath.constData(), O_RDWR | O_CLOEXEC);
        ASSERT_GE(deviceFd.fd, 0);
        device.reset(gbm_create_device(deviceFd.fd));
        ASSERT_TRUE(device);
        renderer = render::EglRenderer::create(device.get(), DRM_FORMAT_ARGB8888);
        ASSERT_TRUE(renderer);
        gbmSurface.reset(gbm_surface_create(device.get(), 1920, 1080, DRM_FORMAT_ARGB8888,
                                            GBM_BO_USE_RENDERING));
        ASSERT_TRUE(gbmSurface);
        target = renderer->createTarget(gbmSurface.get(), {1920, 1080});
        ASSERT_TRUE(target);
    }
    render::TargetRetirement retire {renderer.get(), target.get()};
    ASSERT_TRUE(renderer->beginFrame(target.get()));
    clipboard_test::Harness h;
    compositor::Output output;
    output.pixelSize = {1920, 1080};
    output.scale = *compositor::OutputScale::parse(QString::number(bufferScale));
    h.compositor.setOutputScale(output.scale);
    h.scene.setSize(output.logicalSize());
    decoration::Theme theme;
    decoration::DecorationProvider frames(&theme);
    wm::WindowManager manager(&h.compositor, &h.seat);
    manager.setOutputBounds({QPoint(), output.logicalSize()});
    manager.setFrameProvider(&frames);
    decoration::ShadowProvider shadows(&theme);
    wm::WmShadowControllerDelegate delegate;
    wm::ShadowController shadowController(&manager, &shadows, &delegate);
    wayland::WaylandXdgShell xdg(&h.compositor, &manager, &h.seat, h.server.serialTracker());

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
    ASSERT_TRUE(h.server.addSocket(QStringLiteral("window-visual-toolkit")));
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
    env.insert("WAYLAND_DISPLAY", h.server.socketName());
    env.insert("XDG_RUNTIME_DIR", runtime.path());
    env.insert("XDG_CONFIG_HOME", runtime.path() + "/config");
    env.insert("QT_QPA_PLATFORM", "wayland");
    env.insert("CUTEFISH_TEST_SLOW_PAINT", "1");
    env.insert("GDK_BACKEND", "wayland");
    env.insert("GSK_RENDERER", "cairo");
    env.insert("NO_AT_BRIDGE", "1");
    env.insert("WAYLAND_DEBUG", "1");
    if (interactiveResize && !eglResize)
        env.insert("QT_QUICK_BACKEND", "software");
    if (eglResize) {
        env.remove("QT_QUICK_BACKEND");
        env.insert("QSG_RHI_BACKEND", "opengl");
        const auto libraries = qEnvironmentVariable("CUTEFISH_TEST_QT_WAYLAND_LIBRARY_PATH");
        if (libraries != "system")
            env.insert("LD_LIBRARY_PATH", libraries);
    }
    if (exerciseDamage) {
        env.insert("CUTEFISH_TEST_SCROLL", "1");
        env.insert("QT_QUICK_BACKEND", "software");
    }
    client.setProcessEnvironment(env);
    for (auto &arg : arguments)
        arg.replace("@PROFILE@", runtime.path() + "/profile");
    client.start(program, arguments);
    QByteArray log;
    uint64_t generation = 0;
    ASSERT_TRUE(h.until(
            [&] {
                log += client.readAllStandardError();
                h.compositor.updateTextures(*renderer);
                h.compositor.frameSubmitted(++generation);
                h.compositor.frameDrawn(wayland::nowInMilliseconds(), generation);
                wayland::updateOutputMembership(outputGlobal);
                return !manager.windows().empty()
                        && manager.windows().front()->rootSurface()->hasContents();
            },
            15000))
            << log.constData();
    auto *window = manager.windows().front();
    const auto checkRoundedCorners = [&] {
        const auto visual = wm::WindowVisualController::evaluate(*window);
        const QRectF body = visual.shape.bounds.translated(window->bounds().topLeft());
        const double scale = renderer->rasterScale();
        const QRect pixels(qRound(body.x() * scale), qRound(body.y() * scale),
                           qRound(body.width() * scale), qRound(body.height() * scale));
        for (const auto corner :
             {pixels.topLeft(), pixels.topRight(), pixels.bottomLeft(), pixels.bottomRight()}) {
            for (int dx : {0, 2, 4, 6}) {
                for (int dy : {0, 2, 4, 6}) {
                    const QPoint sample = corner
                            + QPoint(corner.x() == pixels.left() ? dx : -dx,
                                     corner.y() == pixels.top() ? dy : -dy);
                    const QPointF local = (QPointF(sample) + QPointF(.5, .5)) / scale
                            - window->bounds().topLeft();
                    if (!QRect(0, 0, 1920, 1080).contains(sample) || visual.shape.contains(local))
                        continue;
                    uint8_t pixel[4] = {};
                    glReadPixels(sample.x(), 1080 - sample.y() - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                 pixel);
                    EXPECT_LT(pixel[3], 240) << "corner=" << sample.x() << ',' << sample.y();
                }
            }
        }
    };
    if (expected == scene::WindowVisualMode::ServerFrame) {
        const auto oldShape = wm::WindowVisualController::evaluate(*window).shape.bounds;
        const auto nextContent = window->rootSurface()->contentSize() + QSize(40, 24);
        window->requestBounds(
                {window->bounds().topLeft(), window->frameInsets().outset(nextContent)});
        EXPECT_EQ(wm::WindowVisualController::evaluate(*window).shape.bounds, oldShape);
        ASSERT_TRUE(h.until(
                [&] {
                    log += client.readAllStandardError();
                    h.compositor.updateTextures(*renderer);
                    h.compositor.frameSubmitted(++generation);
                    h.compositor.frameDrawn(wayland::nowInMilliseconds(), generation);
                    return window->rootSurface()->contentSize() == nextContent;
                },
                5000))
                << log.constData();
        EXPECT_EQ(wm::WindowVisualController::evaluate(*window).shape.bounds.size(),
                  QSizeF(window->bounds().size()));
    }
    if (interactiveResize) {
        static_cast<scene::WindowAnimation *>(window->node())->finish();
        auto *surface = window->rootSurface();
        const QRect initial = window->bounds();
        const QPoint fixedCorner = surface->node()->absoluteGeometry().bottomRight();
        ASSERT_TRUE(manager.eventHandler()->beginResize(window, wm::ResizeEdge::TopLeft,
                                                        initial.topLeft()));
        QElapsedTimer timer;
        timer.start();
        int step = 0;
        ASSERT_TRUE(h.until(
                [&] {
                    log += client.readAllStandardError();
                    if (step < 30 && timer.elapsed() >= step * 12) {
                        ++step;
                        manager.eventHandler()->onPointerMotion(initial.topLeft()
                                                                - QPoint(step * 3, step * 2));
                    }
                    h.compositor.updateTextures(*renderer);
                    if (eglResize)
                        EXPECT_EQ(surface->bufferSize(), surface->contentSize() * bufferScale);
                    renderer->clear({0, 0, 0, 0});
                    window->node()->render(*renderer);
                    checkRoundedCorners();
                    uint8_t pixel[4] = {};
                    const QPoint sample = fixedCorner - QPoint(20, 20);
                    glReadPixels(sample.x(), 1080 - sample.y() - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                 pixel);
                    EXPECT_GT(pixel[3], 240);
                    EXPECT_EQ(surface->node()->absoluteGeometry().bottomRight(), fixedCorner)
                            << "step=" << step << " buffer=" << surface->bufferSize().width();
                    h.compositor.frameSubmitted(++generation);
                    h.compositor.frameDrawn(wayland::nowInMilliseconds(), generation);
                    return step == 30 && surface->contentSize() == window->bounds().size();
                },
                5000))
                << log.constData();
        manager.eventHandler()->completeDrag(wm::ToplevelEventHandler::DragResult::Success);
    }
    if (exerciseResize) {
        static_cast<scene::WindowAnimation *>(window->node())->finish();
        const QSize baseSize = window->bounds().size();
        for (int step = 0; step < 8; ++step) {
            const QSize nextSize = baseSize + (step % 2 ? QSize(30, 20) : QSize(100, 80));
            const QRect initial = window->bounds();
            const auto edge = step % 2 ? wm::ResizeEdge::TopLeft : wm::ResizeEdge::BottomRight;
            const QPoint start = step % 2 ? initial.topLeft() : initial.bottomRight();
            ASSERT_TRUE(manager.eventHandler()->beginResize(window, edge, start));
            const QPoint delta(nextSize.width() - initial.width(),
                               nextSize.height() - initial.height());
            manager.eventHandler()->onPointerMotion(start + (step % 2 ? -delta : delta));
            ASSERT_TRUE(h.until(
                    [&] {
                        log += client.readAllStandardError();
                        h.compositor.updateTextures(*renderer);
                        auto *surface = window->rootSurface();
                        auto *texture = surface->node()->texture();
                        EXPECT_NE(texture, nullptr);
                        if (texture)
                            EXPECT_EQ(texture->size(), surface->bufferSize());
                        renderer->clear({0, 0, 0, 0});
                        window->node()->render(*renderer);
                        checkRoundedCorners();
                        const QPoint center = window->bounds().center();
                        uint8_t pixel[4] = {};
                        glReadPixels(center.x(), 1080 - center.y() - 1, 1, 1, GL_RGBA,
                                     GL_UNSIGNED_BYTE, pixel);
                        EXPECT_GT(pixel[3], 240)
                                << "step=" << step << " bounds=" << window->bounds().width()
                                << " surface=" << window->rootSurface()->contentSize().width()
                                << " texture=" << window->rootSurface()->node()->texture()
                                << " buffer=" << window->rootSurface()->buffer().get()
                                << " visible="
                                << window->rootSurface()->node()->isEffectivelyVisible();
                        h.compositor.frameSubmitted(++generation);
                        h.compositor.frameDrawn(wayland::nowInMilliseconds(), generation);
                        return window->rootSurface()->contentSize()
                                == window->clientExteriorInsets().outset(nextSize);
                    },
                    5000))
                    << log.constData();
            manager.eventHandler()->completeDrag(wm::ToplevelEventHandler::DragResult::Success);
        }
    }
    h.compositor.updateTextures(*renderer);
    ASSERT_NE(window->rootSurface()->node()->texture(), nullptr);
    static_cast<scene::WindowAnimation *>(window->node())->finish();
    renderer->clear({0.2f, 0.3f, 0.4f, 1});
    window->node()->render(*renderer);
    QImage image(1920, 1080, QImage::Format_RGBA8888);
    glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    ASSERT_EQ(glGetError(), GLenum(GL_NO_ERROR));
    const auto artifacts = qgetenv("CUTEFISH_VISUAL_ARTIFACT_DIR");
    if (!artifacts.isEmpty()) {
        const QString path =
                QString::fromLocal8Bit(artifacts) + "/" + QFileInfo(program).fileName();
        EXPECT_TRUE(image.mirrored().save(path + ".png"));
        const auto original = manager.visualStyle();
        for (bool light : {true, false}) {
            auto preview = original;
            const float value = light ? 1.f : 0.f;
            preview.activeOutline = preview.inactiveOutline = {value, value, value, 0.14f};
            manager.setVisualStyle(preview);
            renderer->clear({0.2f, 0.3f, 0.4f, 1});
            window->node()->render(*renderer);
            glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
            ASSERT_EQ(glGetError(), GLenum(GL_NO_ERROR));
            EXPECT_TRUE(
                    image.mirrored().save(path + (light ? "-light-edge.png" : "-dark-edge.png")));
        }
        manager.setVisualStyle(original);
    }
    const auto state = wm::WindowVisualController::evaluate(*window);
    EXPECT_EQ(state.mode, expected) << log.constData();
    EXPECT_EQ(window->isDecorated(), expected == scene::WindowVisualMode::ServerFrame);
    EXPECT_TRUE(state.clipContent);
    EXPECT_TRUE(state.drawOutline);
    EXPECT_EQ(state.shape.radius, 14);
    EXPECT_NE(shadowController.shadowForWindow(window)->elevation(), wm::ShadowElevation::None);
    if (expected == scene::WindowVisualMode::ServerExterior) {
        EXPECT_TRUE(window->frameInsets().isNull());
        EXPECT_EQ(window->contentBounds(), window->bounds());
    }
    if (exerciseDamage) {
        renderer->setOutputContext(
                {"toolkit", QRect(QPoint(), output.logicalSize()), {1920, 1080}, bufferScale});
        scene::DamageTracker damageTracker;
        int partialFrames = 0;
        for (int frame = 0; frame < 8; ++frame) {
            const auto revision = window->rootSurface()->node()->contentRevision();
            ASSERT_TRUE(h.until(
                    [&] {
                        log += client.readAllStandardError();
                        h.compositor.updateTextures(*renderer);
                        h.compositor.frameSubmitted(++generation);
                        h.compositor.frameDrawn(wayland::nowInMilliseconds(), generation);
                        return window->rootSurface()->node()->contentRevision() != revision;
                    },
                    5000))
                    << log.constData();
            const auto damage = damageTracker.update(
                    *h.scene.root(), renderer->outputContext().logicalViewport, bufferScale);
            const auto physical =
                    render::physicalDamage(damage, renderer->outputContext(), {1920, 1080});
            partialFrames += physical.width() * physical.height() < 1920 * 1080;
            render::EglRendererTestAccess::applyRepaint(*renderer, physical);
            renderer->clear(h.scene.background());
            h.scene.root()->render(*renderer);
            QImage partial({1920, 1080}, QImage::Format_RGBA8888_Premultiplied);
            glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, partial.bits());
            glDisable(GL_SCISSOR_TEST);
            renderer->clear(h.scene.background());
            h.scene.root()->render(*renderer);
            QImage complete({1920, 1080}, QImage::Format_RGBA8888_Premultiplied);
            glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, complete.bits());
            EXPECT_EQ(partial, complete) << "frame=" << frame << " scale=" << bufferScale;
        }
        EXPECT_GT(partialFrames, 0);
        std::printf("DAMAGE renderer=%s scale=%.2f partial=%d/8\n", glGetString(GL_RENDERER),
                    bufferScale, partialFrames);
    }
    std::printf("OBSERVED %s mode=%d client=%dx%d exterior=%d,%d,%d,%d\n",
                program.toLocal8Bit().constData(), int(state.mode), window->contentBounds().width(),
                window->contentBounds().height(), window->clientExteriorInsets().left,
                window->clientExteriorInsets().top, window->clientExteriorInsets().right,
                window->clientExteriorInsets().bottom);
}

} // namespace

TEST(WindowVisualToolkit, QtSsd)
{
    acceptClient(QStringLiteral(QT_SCALE_CLIENT), {}, scene::WindowVisualMode::ServerFrame);
}

TEST(WindowVisualToolkit, KonsoleSsd)
{
    const auto konsole = QStandardPaths::findExecutable("konsole");
    if (konsole.isEmpty())
        GTEST_SKIP() << "Konsole is not installed";
    acceptClient(konsole, {"--separate", "--nofork"}, scene::WindowVisualMode::ServerFrame);
}

TEST(WindowVisualToolkit, GtkCsd)
{
    const auto gtk = QStandardPaths::findExecutable("gtk3-demo");
    if (gtk.isEmpty())
        GTEST_SKIP() << "gtk3-demo is not installed";
    acceptClient(gtk, {}, scene::WindowVisualMode::ServerExterior, true);
}

TEST(WindowVisualToolkit, Gtk4CsdResize)
{
    const auto gtk = QStandardPaths::findExecutable("gtk4-demo");
    if (gtk.isEmpty())
        GTEST_SKIP() << "gtk4-demo is not installed";
    acceptClient(gtk, {}, scene::WindowVisualMode::ServerExterior, true);
}

TEST(WindowVisualToolkit, ChromiumClientChrome)
{
    const auto chromium = QStandardPaths::findExecutable("chromium");
    if (chromium.isEmpty())
        GTEST_SKIP() << "Chromium is not installed";
    acceptClient(chromium,
                 {"--ozone-platform=wayland", "--disable-gpu", "--no-first-run",
                  "--no-default-browser-check", "--disable-background-networking",
                  "--disable-dev-shm-usage", "--user-data-dir=@PROFILE@", "--window-size=640,480",
                  "about:blank"},
                 scene::WindowVisualMode::ServerExterior, true);
}

TEST(WindowVisualToolkit, ElectronClientChrome)
{
    const auto electron = QStandardPaths::findExecutable("electron");
    if (electron.isEmpty())
        GTEST_SKIP() << "Electron is not installed";
    acceptClient(electron,
                 {QStringLiteral(ELECTRON_VISUAL_CLIENT), "--ozone-platform=wayland",
                  "--disable-gpu", "--user-data-dir=@PROFILE@"},
                 scene::WindowVisualMode::ServerExterior);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(WindowVisualToolkit, FishUiInteractiveResize)
{
    if (!QFileInfo::exists(QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + "/FishUI/qmldir"))
        GTEST_SKIP() << "FishUI is not installed";
    acceptClient(QStringLiteral(FISHUI_RESIZE_CLIENT), {}, scene::WindowVisualMode::ServerExterior,
                 false, true);
}

TEST(WindowVisualToolkit, FishUiEglInteractiveResize)
{
    if (!qEnvironmentVariableIsSet("CUTEFISH_TEST_QT_WAYLAND_LIBRARY_PATH"))
        GTEST_SKIP() << "Set CUTEFISH_TEST_QT_WAYLAND_LIBRARY_PATH to the test runtime or system";
    for (const double scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        SCOPED_TRACE(scale);
        acceptClient(QStringLiteral(FISHUI_RESIZE_CLIENT), {},
                     scene::WindowVisualMode::ServerExterior, false, true, true, scale);
    }
}

TEST(WindowVisualToolkit, QtScrollingDamage)
{
    for (double scale : {1.25, 1.5})
        acceptClient(QStringLiteral(QT_SCALE_CLIENT), {}, scene::WindowVisualMode::ServerFrame,
                     false, false, false, scale, true);
}

TEST(WindowVisualToolkit, QuickScrollingDamage)
{
    for (double scale : {1.25, 1.5})
        acceptClient(QStringLiteral(QT_QUICK_SCALE_CLIENT), {},
                     scene::WindowVisualMode::ServerFrame, false, false, false, scale, true);
}

TEST(WindowVisualToolkit, FishUiScrollingDamage)
{
    if (!QFileInfo::exists(QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + "/FishUI/qmldir"))
        GTEST_SKIP() << "FishUI is not installed";
    for (double scale : {1.25, 1.5})
        acceptClient(QStringLiteral(FISHUI_RESIZE_CLIENT), {},
                     scene::WindowVisualMode::ServerExterior, false, true, false, scale, true);
}

TEST(WindowVisualToolkit, ChromiumScrollingDamage)
{
    const auto chromium = QStandardPaths::findExecutable("chromium");
    if (chromium.isEmpty())
        GTEST_SKIP() << "Chromium is not installed";
    const QByteArray html =
            "<body style='height:10000px;background:repeating-linear-gradient(white 0px,white "
            "20px,blue 20px,blue 40px)'><script>setInterval(()=>scrollBy(0,9),50)</script>";
    const QString page = "data:text/html,"
            + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(html)));
    for (double scale : {1.25, 1.5})
        acceptClient(chromium,
                     {"--ozone-platform=wayland", "--disable-gpu", "--no-first-run",
                      "--no-default-browser-check", "--disable-background-networking",
                      "--disable-dev-shm-usage", "--user-data-dir=@PROFILE@",
                      "--window-size=640,480", page},
                     scene::WindowVisualMode::ServerExterior, true, false, false, scale, true);
}
