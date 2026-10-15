// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The lock screen's panel drawn by Skia on the renderer's own GL context, into
// a texture the renderer owns, against the same frame rendered on the CPU.

#include <GLES3/gl3.h>
#include <gtest/gtest.h>

#include <QGuiApplication>

#include "auth/auth_controller.h"
#include "auth/auth_view.h"
#include "egl_test_util.h"
#include "fake_backend.h"
#include "render/gl_texture.h"
#include "toolkit/toolkit_host.h"
#include "ui/embedded_window.h"

using namespace cutefish;
using namespace cutefish::render;

namespace {

constexpr QSize kLogical(480, 600);

class PanelGpuPixels : public testing::TestWithParam<double>
{
protected:
    void SetUp() override
    {
        renderer = EglRendererTestAccess::create();
        ASSERT_TRUE(renderer);
        native = QSize(qRound(kLogical.width() * scale()), qRound(kLogical.height() * scale()));
        const EGLint attributes[] = {EGL_WIDTH, native.width(), EGL_HEIGHT, native.height(),
                                     EGL_NONE};
        const auto surface =
                eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attributes);
        ASSERT_NE(surface, EGL_NO_SURFACE);
        target = std::make_unique<EglRenderTarget>(renderer->eglDisplay(), surface, nullptr, native);

        toolkit::application();
        controller = std::make_unique<auth::AuthController>(auth::AuthController::Lock, &backend);
        window = std::make_unique<ui::EmbeddedWindow>();
        auto screen = std::make_unique<auth::AuthScreen>(window.get(), controller.get());
        screen->setClockForTesting(1792028460);
        view = screen.get();
        window->setContentView(std::move(screen));
        view->focusPassword();
        window->setScale(scale());
        window->setSize({double(kLogical.width()), double(kLogical.height())});
        panel = std::make_unique<toolkit::PanelTexture>(window.get());
    }

    void TearDown() override
    {
        if (renderer && target) {
            ASSERT_TRUE(renderer->beginFrame(target.get()));
            panel->releaseRenderResources();
            renderer->endFrame();
        }
        panel.reset();
        window.reset();
        if (renderer && target)
            renderer->releaseTarget(target.get());
        target.reset();
        renderer.reset();
    }

    double scale() const { return GetParam(); }

    // The panel's texture drawn 1:1 over transparency, as the output's frame
    // captures it.
    QImage drawn(bool dirty)
    {
        renderer->setOutputContext({QStringLiteral("out"), QRect(QPoint(), kLogical), native,
                                    scale(), 0, ++frame});
        EXPECT_TRUE(renderer->beginFrame(target.get()));
        const QImage image = renderer->captureFrame([&] {
            renderer->clear({0, 0, 0, 0});
            Texture *texture = panel->texture(*renderer, dirty);
            EXPECT_TRUE(texture);
            if (texture)
                renderer->drawTextureMapped(texture, QRect(QPoint(), kLogical), 1.0f, QTransform());
        });
        renderer->cancelFrame();
        return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }

    struct Difference {
        int worst = 0;
        int over2 = 0;
        QRect where;
    };

    // Over the whole image, or only inside or outside `region` (device pixels).
    static Difference difference(const QImage &a, const QImage &b, const QRect &region = {},
                                 bool inside = false)
    {
        Difference result;
        for (int y = 0; y < a.height(); ++y) {
            const uchar *left = a.constScanLine(y);
            const uchar *right = b.constScanLine(y);
            for (int x = 0; x < a.width(); ++x) {
                if (!region.isEmpty() && region.contains(x, y) != inside)
                    continue;
                int pixel = 0;
                for (int c = 0; c < 4; ++c)
                    pixel = std::max(pixel, std::abs(int(left[x * 4 + c]) - int(right[x * 4 + c])));
                result.worst = std::max(result.worst, pixel);
                if (pixel > 2) {
                    ++result.over2;
                    result.where |= QRect(x, y, 1, 1);
                }
            }
        }
        return result;
    }

    std::unique_ptr<EglRenderer> renderer;
    std::unique_ptr<EglRenderTarget> target;
    QSize native;
    uint64_t frame = 0;
    FakeBackend backend;
    std::unique_ptr<auth::AuthController> controller;
    std::unique_ptr<ui::EmbeddedWindow> window;
    std::unique_ptr<toolkit::PanelTexture> panel;
    auth::AuthScreen *view = nullptr;
};

// Skia's GPU backend antialiases paths its own way: the submit button's
// 1.5-px arrow is compared on its own, more loosely. Its colour also animates
// between the frames once there is text to submit.
TEST_P(PanelGpuPixels, MatchesTheCpuFrameWholeAndAfterAChange)
{
    window->setKeyboardFocus(true);
    window->updateLayout();
    const QRectF field = view->fieldBounds();
    const QRect button = QRectF((field.right() - 28) * scale(), (field.top() + 4) * scale(),
                                24 * scale(), 24 * scale())
                                 .toAlignedRect()
                                 .adjusted(-1, -1, 1, 1);
    const auto expectClose = [&](const QImage &cpu, const QImage &gpu, int buttonLevels) {
        const Difference rest = difference(cpu, gpu, button, false);
        EXPECT_LE(rest.worst, 2) << rest.over2 << " pixels over 2 in " << rest.where.x() << ","
                                 << rest.where.y() << " " << rest.where.width() << "x"
                                 << rest.where.height();
        EXPECT_LE(difference(cpu, gpu, button, true).worst, buttonLevels);
    };

    ASSERT_TRUE(panel->renderImage());
    const QImage cpu = panel->image().convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage gpu = drawn(true);
    ASSERT_TRUE(panel->lastFrameOnGpu());
    ASSERT_EQ(cpu.size(), gpu.size());
    expectClose(cpu, gpu, 32);

    // A typed character repaints only what changed, on top of the last frame.
    ui::KeyEvent key;
    key.key = 'a';
    key.text = "a";
    window->key(key, true);
    window->key(key, false);
    const QImage typed = drawn(true);
    ASSERT_TRUE(panel->lastFrameOnGpu());
    EXPECT_GT(difference(gpu, typed, button, false).worst, 32);
    // The CPU frame after GPU ones, as a fallback would take it, is the same.
    panel->renderImage();
    expectClose(panel->image().convertToFormat(QImage::Format_RGBA8888_Premultiplied), typed, 255);
}

INSTANTIATE_TEST_SUITE_P(Scales, PanelGpuPixels, testing::Values(1.0, 1.5, 1.75, 2.0));

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
