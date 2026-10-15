// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The application switcher composited as an output draws it: its shadow, its
// glass, the panel's texture, the outline and a node drawn after it, through
// the real GL renderer, with the panel drawn on the GPU against the CPU.

#include <GLES3/gl3.h>
#include <gtest/gtest.h>

#include <QGuiApplication>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "decoration/decoration_theme.h"
#include "display/display_manager.h"
#include "egl_test_util.h"
#include "fake_renderer.h"
#include "input/seat.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "shm_test_util.h"
#include "switcher/switcher_view.h"
#include "toolkit/toolkit_host.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace cutefish;
using namespace cutefish::render;

namespace {

constexpr QRect kOutput(0, 0, 640, 320);

// A node drawn after the switcher, as anything above it in the scene is.
class MarkerNode final : public scene::Node
{
public:
    MarkerNode()
        : Node(QStringLiteral("marker"))
    {
    }

protected:
    void paint(Renderer &renderer, const QRect &bounds) const override
    {
        renderer.fillRect(bounds, {0, 0.5f, 0, 0.5f});
    }
};

struct Param {
    double scale;
    bool dark;
};

class SwitcherGpuPixels : public testing::TestWithParam<Param>
{
protected:
    void SetUp() override
    {
        const double scale = GetParam().scale;
        renderer = EglRendererTestAccess::create();
        ASSERT_TRUE(renderer);
        native = QSize(qRound(kOutput.width() * scale), qRound(kOutput.height() * scale));
        const EGLint attributes[] = {EGL_WIDTH, native.width(), EGL_HEIGHT, native.height(),
                                     EGL_NONE};
        const auto surface =
                eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attributes);
        ASSERT_NE(surface, EGL_NO_SURFACE);
        target = std::make_unique<EglRenderTarget>(renderer->eglDisplay(), surface, nullptr, native);

        manager.setOutputBounds(kOutput);
        display::DisplaySnapshot physical;
        physical.sessionId = display::DisplayId(QStringLiteral("a"));
        physical.persistentKey = QStringLiteral("edid-a");
        physical.portKey = QStringLiteral("port-a");
        physical.connectorName = QStringLiteral("a");
        physical.devicePath = QStringLiteral("/dev/dri/card0");
        physical.physicalSizeMm = {600, 340};
        physical.modes = {{native, 60000, 148500, native.width() + 100, native.height() + 50, 0,
                           QStringLiteral("mode"), true}};
        physical.currentMode = physical.modes.first();
        ASSERT_TRUE(displays.installHardware(
                {QStringLiteral("instance"), QStringLiteral("seat0"), 1, {physical}}));
        const display::LogicalDisplay logical {
                display::DisplayId(QStringLiteral("a")),
                {display::DisplayId(QStringLiteral("a"))},
                QStringLiteral("Cutefish-a"),
                kOutput,
                kOutput,
                *compositor::OutputScale::fromNumerator(uint32_t(qRound(scale * 120))),
                display::DisplayRotation::Normal,
                true};
        ASSERT_TRUE(displays.installApplied({QStringLiteral("instance"), 1, 1, {}, {logical}, {}}));
        manager.setDisplayManager(&displays);
        theme.setDarkMode(GetParam().dark);
        view = std::make_unique<switcher::SwitcherView>(switcherContainer, &manager, &theme);
        for (const char *id : {"one", "two", "three"})
            windows.push_back(open(QString::fromLatin1(id)));
        marker = scene.root()->addChild(std::make_unique<MarkerNode>());
    }

    void TearDown() override
    {
        toolkit::setGpuRenderingAllowed(true);
        if (renderer && target) {
            renderer->setOutputContext(context());
            ASSERT_TRUE(renderer->beginFrame(target.get()));
            view->releaseRenderResources();
            renderer->cancelFrame();
        }
        view.reset();
        manager.setDisplayManager(nullptr);
        seat.shutdown();
        if (renderer && target)
            renderer->releaseTarget(target.get());
        target.reset();
        renderer.reset();
    }

    wm::Window *open(const QString &id)
    {
        owned.push_back(compositor.createSurface());
        auto *surface = owned.back().get();
        surface->attach(pool->createBuffer({200, 100}, 0, 800, PixelFormat::Bgrx8888, nullptr), {});
        surface->commit();
        compositor.updateTextures(fake);
        return manager.createWindow(surface, {.requestedSize = {200, 100}, .title = id,
                                              .applicationId = id});
    }

    OutputRenderContext context()
    {
        return {QStringLiteral("out"), kOutput, native, GetParam().scale, 0, ++frame};
    }

    // A busy backdrop, the switcher over it and a node after it, as an output
    // draws them.
    QImage composite()
    {
        renderer->setOutputContext(context());
        EXPECT_TRUE(renderer->beginFrame(target.get()));
        const QImage image = renderer->captureFrame([&] {
            renderer->clear({0.2f, 0.4f, 0.8f, 1});
            for (int x = 0; x < kOutput.width(); x += 24)
                renderer->fillRect(QRect(x, 0, 12, kOutput.height()), {0.95f, 0.85f, 0.2f, 1});
            for (int y = 0; y < kOutput.height(); y += 40)
                renderer->fillRect(QRect(0, y, kOutput.width(), 6), {0.1f, 0.1f, 0.1f, 1});
            scene.root()->render(*renderer);
        });
        renderer->cancelFrame();
        return image;
    }

    static int differing(const QImage &a, const QImage &b, int threshold, QRect *where)
    {
        int count = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                const int d = std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                                        std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
                if (d > threshold) {
                    ++count;
                    *where |= QRect(x, y, 1, 1);
                }
            }
        }
        return count;
    }

    std::unique_ptr<EglRenderer> renderer;
    std::unique_ptr<EglRenderTarget> target;
    QSize native;
    uint64_t frame = 0;
    scene::Scene scene;
    scene::Node *surfaces = scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    scene::Node *switcherContainer = scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("switcher")));
    scene::Node *marker = nullptr;
    compositor::Compositor compositor {surfaces};
    testsupport::FakeRenderer fake;
    input::Seat seat {&compositor};
    wm::WindowManager manager {&compositor, &seat};
    display::DisplayManager displays;
    decoration::Theme theme;
    std::vector<std::unique_ptr<compositor::Surface>> owned;
    std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 20);
    std::vector<wm::Window *> windows;
    std::unique_ptr<switcher::SwitcherView> view;
};

TEST_P(SwitcherGpuPixels, GpuPanelCompositesAsTheCpuOne)
{
    view->showWindowCycle(windows, 1, QStringLiteral("Cutefish-a"));
    marker->setGeometry(view->bounds().adjusted(-20, -20, 20, 20));

    toolkit::setGpuRenderingAllowed(false);
    const QImage cpu = composite();
    view->hideWindowCycle();
    view->showWindowCycle(windows, 1, QStringLiteral("Cutefish-a"));
    toolkit::setGpuRenderingAllowed(true);
    const QImage gpu = composite();
    // Twice: the second frame reuses the texture the first one drew.
    const QImage again = composite();

    QRect where;
    const int whole = differing(cpu, gpu, 2, &where);
    EXPECT_EQ(whole, 0) << "pixels over 2 in " << where.x() << "," << where.y() << " "
                        << where.width() << "x" << where.height();
    QRect whereAgain;
    EXPECT_EQ(differing(gpu, again, 2, &whereAgain), 0)
            << "in " << whereAgain.x() << "," << whereAgain.y() << " " << whereAgain.width() << "x"
            << whereAgain.height();
}

INSTANTIATE_TEST_SUITE_P(States, SwitcherGpuPixels,
                         testing::Values(Param {1.0, false}, Param {1.0, true}, Param {1.5, false},
                                         Param {1.5, true}));

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
