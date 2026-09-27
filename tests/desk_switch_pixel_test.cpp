// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The desk switch animation through the real GL renderer: the ending desk is
// copied inside a frame, and the strip lands on it pixel for pixel.

#include <GLES3/gl3.h>
#include <gtest/gtest.h>

#include "egl_test_util.h"
#include "scene/desk_switch_node.h"
#include "scene/scene.h"

using namespace cutefish;
using namespace cutefish::render;

namespace {

constexpr QRect kViewport(0, 0, 32, 16);
constexpr QSize kNative(64, 32);

class DeskSwitchPixels : public testing::Test
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
        left->setGeometry(QRect(0, 0, 16, 16));
        right->setGeometry(QRect(16, 0, 16, 16));
        node->setGeometry(kViewport);
    }

    void TearDown() override
    {
        node->stop();
        if (renderer && target)
            renderer->releaseTarget(target.get());
        target.reset();
        renderer.reset();
    }

    // Straight into the output's frame, as the compositor draws it.
    QImage paint()
    {
        renderer->setOutputContext({QStringLiteral("out"), kViewport, kNative, 2, 0, 1});
        renderer->clear({1, 0, 1, 1});
        node->render(*renderer);
        QImage image(kNative, QImage::Format_RGBA8888);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, kNative.width(), kNative.height(), GL_RGBA, GL_UNSIGNED_BYTE,
                     image.bits());
        return image.mirrored(false, true);
    }

    qint64 now = 0;
    scene::Scene scene;
    scene::Node *container =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    scene::ColorNode *left = static_cast<scene::ColorNode *>(
            container->addChild(std::make_unique<scene::ColorNode>(Color {1, 0, 0, 1})));
    scene::ColorNode *right = static_cast<scene::ColorNode *>(
            container->addChild(std::make_unique<scene::ColorNode>(Color {0, 1, 0, 1})));
    scene::DeskSwitchNode *node = static_cast<scene::DeskSwitchNode *>(
            container->addChild(std::make_unique<scene::DeskSwitchNode>([this] {
                return now;
            })));
    std::unique_ptr<EglRenderer> renderer;
    std::unique_ptr<EglRenderTarget> target;
};

} // namespace

TEST_F(DeskSwitchPixels, TheStripLandsOnTheEndingDeskCopiedInsideAFrame)
{
    // The starting desk, taken the way the controller takes it.
    std::unique_ptr<Texture> starting;
    ASSERT_TRUE(renderer->renderToTexture(starting, kViewport, [&] {
        node->paintCoveredContent(*renderer);
    }));
    scene::DeskSwitchNode::Frames frames;
    frames[QStringLiteral("out")] = std::move(starting);
    node->start(scene::DeskSwitchNode::Type::Continuous, 2, 0, 1, std::move(frames),
                kViewport.width());

    // The ending desk is activated underneath.
    left->setColor({0, 0, 1, 1});
    right->setColor({1, 1, 1, 1});
    node->takeEndingScreenshot(1);

    QImage image = paint();
    ASSERT_FALSE(image.isNull());
    EXPECT_TRUE(node->endingScreenshotTaken());
    // Still the starting desk, whatever is live below.
    EXPECT_EQ(image.pixelColor(8, 16), QColor(Qt::red));
    EXPECT_EQ(image.pixelColor(56, 16), QColor(Qt::green));

    node->startAnimation();
    now += 200;
    image = paint();
    // Partway: the starting desk has left through the left edge, and the gap
    // between the desks is black.
    EXPECT_EQ(image.pixelColor(0, 16), QColor(Qt::black));

    now += 1000;
    image = paint();
    EXPECT_EQ(image.pixelColor(1, 16), QColor(Qt::blue));
    EXPECT_EQ(image.pixelColor(30, 16), QColor(Qt::blue));
    EXPECT_EQ(image.pixelColor(34, 16), QColor(Qt::white));
    EXPECT_EQ(image.pixelColor(62, 30), QColor(Qt::white));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(DeskSwitchPixels, AnOpaqueFrameReplacesWhatLiesBelowWithoutBlending)
{
    // Half-transparent texels in a texture marked opaque: its alpha is never
    // read, so nothing of the magenta below shows through.
    std::unique_ptr<Texture> frame;
    ASSERT_TRUE(renderer->renderToTexture(frame, kViewport, [&] {
        renderer->clear({0, 0, 0, 0});
        renderer->fillRect(kViewport, {0, 0, 1, 0.5});
    }));
    frame->contentOpaque = true;
    renderer->setOutputContext({QStringLiteral("out"), kViewport, kNative, 2, 0, 1});
    renderer->clear({1, 0, 1, 1});
    renderer->drawTexture(frame.get(), kViewport, 1);
    QImage image(kNative, QImage::Format_RGBA8888);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, kNative.width(), kNative.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    const QColor pixel = image.pixelColor(20, 10);
    EXPECT_EQ(pixel.red(), 0);
    EXPECT_NEAR(pixel.blue(), 128, 1);
    EXPECT_EQ(pixel.alpha(), 255);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}
