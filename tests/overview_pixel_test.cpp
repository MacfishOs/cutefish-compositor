// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The overview through the real GL renderer: a window is drawn from its own
// texture at its place, over the dimmed desktop, and at rest where it was.

#include <GLES3/gl3.h>
#include <gtest/gtest.h>

#include "egl_test_util.h"
#include "scene/overview_node.h"
#include "scene/scene.h"
#include "scene/window_animation.h"

using namespace cutefish;
using namespace cutefish::render;

namespace {

constexpr QRect kViewport(0, 0, 32, 16);
constexpr QSize kNative(64, 32);
constexpr QRect kWindow(0, 0, 16, 16);
constexpr QRectF kPlace(16, 4, 8, 8);

class OverviewPixels : public testing::Test
{
protected:
    void SetUp() override
    {
        renderer = EglRendererTestAccess::create();
        ASSERT_TRUE(renderer);
        const EGLint attributes[] = {EGL_WIDTH, kNative.width(), EGL_HEIGHT, kNative.height(),
                                     EGL_NONE};
        const auto surface =
                eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attributes);
        ASSERT_NE(surface, EGL_NO_SURFACE);
        target = std::make_unique<EglRenderTarget>(renderer->eglDisplay(), surface, nullptr,
                                                   kNative);
        renderer->setOutputContext({QStringLiteral("out"), kViewport, kNative, 2, 0, 1});
        ASSERT_TRUE(renderer->beginFrame(target.get()));

        scene.setSize(kViewport.size());
        desktop->setGeometry(kViewport);
        window->setGeometry(kWindow);
        window->addChild(std::make_unique<scene::ColorNode>(Color {1, 0, 0, 1}))
                ->setGeometry(QRect(QPoint(), kWindow.size()));
        window->finish();
        node->setGeometry(kViewport);
        node->setBackdrop({desktop});
        scene::OverviewNode::Item item;
        item.windows = {window};
        item.to = kPlace;
        node->setItems({item}, false);
    }

    void TearDown() override
    {
        if (renderer && target)
            renderer->releaseTarget(target.get());
        target.reset();
        renderer.reset();
    }

    QImage paint()
    {
        renderer->setOutputContext({QStringLiteral("out"), kViewport, kNative, 2, 0, 1});
        renderer->frameTime = double(now);
        renderer->clear({1, 0, 1, 1});
        container->render(*renderer);
        QImage image(kNative, QImage::Format_RGBA8888);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, kNative.width(), kNative.height(), GL_RGBA, GL_UNSIGNED_BYTE,
                     image.bits());
        return image.mirrored(false, true);
    }

    // Logical coordinates to the pixel at their centre.
    static QPoint pixel(double x, double y) { return QPoint(int(x * 2), int(y * 2)); }

    qint64 now = 1000;
    scene::Scene scene;
    scene::Node *container =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    scene::ColorNode *desktop = static_cast<scene::ColorNode *>(
            container->addChild(std::make_unique<scene::ColorNode>(Color {0, 0, 1, 1})));
    scene::WindowAnimation *window = static_cast<scene::WindowAnimation *>(
            container->addChild(std::make_unique<scene::WindowAnimation>([this] {
                return double(now);
            })));
    scene::OverviewNode *node = static_cast<scene::OverviewNode *>(
            container->addChild(std::make_unique<scene::OverviewNode>([this] {
                return now;
            })));
    std::unique_ptr<EglRenderer> renderer;
    std::unique_ptr<EglRenderTarget> target;
};

} // namespace

TEST_F(OverviewPixels, AtRestTheDeskIsAsItIs)
{
    const QImage image = paint();
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(QColor(Qt::red), image.pixelColor(pixel(8, 8)));
    EXPECT_EQ(QColor(Qt::blue), image.pixelColor(pixel(28, 8)));
}

TEST_F(OverviewPixels, TheWindowIsAtItsPlaceOverTheDimmedDesktop)
{
    node->jumpTo(1);
    const QImage image = paint();
    ASSERT_FALSE(image.isNull());
    // Its place, edge to edge.
    EXPECT_EQ(QColor(Qt::red), image.pixelColor(pixel(20, 8)));
    EXPECT_EQ(QColor(Qt::red), image.pixelColor(QPoint(32, 8)));
    EXPECT_EQ(QColor(Qt::red), image.pixelColor(QPoint(47, 23)));
    // Where it was, and around its place, the desktop dimmed.
    const QColor dimmed = image.pixelColor(pixel(4, 4));
    EXPECT_EQ(0, dimmed.red());
    EXPECT_NEAR(255 * (1 - scene::OverviewNode::kDimOpacity), dimmed.blue(), 2);
    EXPECT_EQ(dimmed, image.pixelColor(QPoint(31, 8)));
    EXPECT_EQ(dimmed, image.pixelColor(QPoint(48, 8)));
    EXPECT_EQ(dimmed, image.pixelColor(QPoint(40, 7)));
    EXPECT_EQ(dimmed, image.pixelColor(QPoint(40, 24)));
}

TEST_F(OverviewPixels, HalfwayTheWindowIsHalfwayThere)
{
    node->animateTo(1);
    // Halfway along the tween, whenever that is.
    while (node->progress(double(now)) < 0.5)
        ++now;
    const double progress = node->progress(double(now));
    const QRectF rect = node->presentedRect(0, progress, double(now));
    const QImage image = paint();
    EXPECT_EQ(QColor(Qt::red), image.pixelColor(pixel(rect.center().x(), rect.center().y())));
    EXPECT_NE(QColor(Qt::red), image.pixelColor(pixel(rect.right() + 1, rect.center().y())));
}
