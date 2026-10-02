// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <GLES3/gl3.h>
#include <gtest/gtest.h>

#include "egl_test_util.h"

using namespace cutefish;

TEST(CapturePixels, IndependentTargetAndRepeatedFrames)
{
    auto renderer = render::EglRendererTestAccess::create();
    ASSERT_NE(renderer, nullptr);
    const EGLint attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 48, EGL_NONE};
    auto surface = eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attrs);
    ASSERT_NE(surface, EGL_NO_SURFACE);
    auto target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), surface,
                                                            nullptr, QSize(64, 48));
    render::TargetRetirement retire {renderer.get(), target.get()};
    renderer->setOutputContext({"test", QRect(100, 50, 64, 48), {64, 48}, 1, 0, 1});
    ASSERT_TRUE(renderer->beginFrame(target.get()));
    renderer->clear({1, 0, 0, 1});
    render::PresentationSubmission submission;
    renderer->presentationSubmission = &submission;
    GLint framebuffer, viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glEnable(GL_SCISSOR_TEST);
    glScissor(20, 20, 2, 2);
    for (int frame = 0; frame < 20; ++frame) {
        const auto image = renderer->captureFrame([&] {
            EXPECT_EQ(renderer->presentationSubmission, nullptr);
            EXPECT_TRUE(renderer->capturing);
            renderer->clear({0, 0, 1, 1});
            renderer->fillRect({100 + frame, 50, 4, 4}, {0, 1, 0, 1});
        });
        ASSERT_EQ(image.size(), QSize(64, 48));
        EXPECT_EQ(image.pixelColor(frame, 0), QColor(Qt::green));
        EXPECT_EQ(image.pixelColor(frame, 47), QColor(Qt::blue));
        EXPECT_EQ(renderer->presentationSubmission, &submission);
        EXPECT_FALSE(renderer->capturing);
        EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
        GLint scissor[4];
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        EXPECT_EQ(scissor[0], 20);
        EXPECT_EQ(scissor[1], 20);
        EXPECT_EQ(scissor[2], 2);
        EXPECT_EQ(scissor[3], 2);
        GLint current, currentViewport[4];
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current);
        glGetIntegerv(GL_VIEWPORT, currentViewport);
        EXPECT_EQ(current, framebuffer);
        for (int i = 0; i < 4; ++i)
            EXPECT_EQ(currentViewport[i], viewport[i]);
        uint8_t pixel[4];
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        EXPECT_EQ(pixel[0], 255);
        EXPECT_EQ(pixel[1], 0);
        EXPECT_EQ(pixel[2], 0);
    }
    renderer->presentationSubmission = nullptr;
    renderer->cancelFrame();
}

TEST(CapturePixels, NativeLetterboxAndTransform)
{
    auto renderer = render::EglRendererTestAccess::create();
    ASSERT_NE(renderer, nullptr);
    const EGLint attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 48, EGL_NONE};
    auto surface = eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attrs);
    ASSERT_NE(surface, EGL_NO_SURFACE);
    auto target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), surface,
                                                            nullptr, QSize(64, 48));
    render::TargetRetirement retire {renderer.get(), target.get()};
    for (uint32_t transform = 0; transform < 4; ++transform) {
        const QSize logical = transform & 1 ? QSize(32, 48) : QSize(48, 32);
        renderer->setOutputContext(
                {"test", QRect(QPoint(), logical), {64, 48}, 1, transform, 1, QRect(8, 8, 48, 32)});
        ASSERT_TRUE(renderer->beginFrame(target.get()));
        const auto image = renderer->captureFrame([&] {
            renderer->clear({0, 0, 0, 1});
            renderer->fillRect(QRect(QPoint(), logical), {0, 1, 0, 1});
            renderer->fillRect(QRect(0, 0, 3, 3), {1, 0, 0, 1});
        });
        ASSERT_EQ(image.size(), QSize(64, 48));
        EXPECT_EQ(image.pixelColor(0, 0), QColor(Qt::black));
        const auto upright = image.transformed(QTransform().rotate(90 * int(transform)));
        EXPECT_EQ(upright.pixelColor(8, 8), QColor(Qt::red));
        EXPECT_EQ(upright.pixelColor(12, 12), QColor(Qt::green));
        EXPECT_EQ(image.pixelColor(56, 40), QColor(Qt::black));
    }
    renderer->cancelFrame();
}

TEST(CapturePixels, AReadbackCollectedLaterMatchesTheWaitingCapture)
{
    auto renderer = render::EglRendererTestAccess::create();
    ASSERT_NE(renderer, nullptr);
    const EGLint attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 48, EGL_NONE};
    auto surface = eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attrs);
    ASSERT_NE(surface, EGL_NO_SURFACE);
    auto target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), surface,
                                                            nullptr, QSize(64, 48));
    render::TargetRetirement retire {renderer.get(), target.get()};
    const render::OutputRenderContext output {"test", QRect(100, 50, 64, 48), {64, 48}, 1, 1, 1};
    renderer->setOutputContext(output);
    ASSERT_TRUE(renderer->beginFrame(target.get()));
    const auto paint = [&] {
        renderer->clear({0, 0, 1, 1});
        renderer->fillRect({110, 55, 20, 6}, {0, 1, 0, 1});
        renderer->fillRect({150, 80, 8, 12}, {1, 0, 0, 1});
    };
    GLint packBuffer = -1;
    const auto frame = renderer->captureFrameLater(paint);
    ASSERT_NE(frame, nullptr);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    EXPECT_EQ(packBuffer, 0);
    const render::OutputRenderContext window {"test", QRect(105, 52, 30, 20), {45, 30}, 1.5, 0, 1};
    const auto image = renderer->captureImageLater(window, {45, 30}, paint);
    ASSERT_NE(image, nullptr);
    const QImage expectedFrame = renderer->captureFrame(paint);
    const QImage expectedImage = renderer->captureImage(window, {45, 30}, paint);
    renderer->cancelFrame();
    ASSERT_FALSE(expectedFrame.isNull());
    EXPECT_EQ(frame->take(), expectedFrame);
    EXPECT_EQ(image->take(), expectedImage);
    // Collected once, for every session that shares it.
    EXPECT_EQ(frame->take(), expectedFrame);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(CapturePixels, ACopiedOutputFrameMatchesThePaintedCapture)
{
    auto renderer = render::EglRendererTestAccess::create();
    ASSERT_NE(renderer, nullptr);
    const EGLint attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 48, EGL_NONE};
    auto surface = eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attrs);
    ASSERT_NE(surface, EGL_NO_SURFACE);
    auto target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), surface,
                                                            nullptr, QSize(64, 48));
    render::TargetRetirement retire {renderer.get(), target.get()};
    for (uint32_t transform = 0; transform < 4; ++transform) {
        const QSize logical = transform & 1 ? QSize(32, 48) : QSize(48, 32);
        renderer->setOutputContext(
                {"test", QRect(QPoint(), logical), {64, 48}, 1, transform, 1, QRect(8, 8, 48, 32)});
        ASSERT_TRUE(renderer->beginFrame(target.get()));
        const auto paint = [&] {
            renderer->clear({0, 0, 0, 1});
            renderer->fillRect(QRect(QPoint(), logical), {0, 1, 0, 1});
            renderer->fillRect(QRect(0, 0, 3, 5), {1, 0, 0, 1});
        };
        paint();
        EXPECT_FALSE(renderer->copyOutputFrame({64, 48}));
        // A partial repaint leaves its scissor on; the copy takes the whole frame.
        glEnable(GL_SCISSOR_TEST);
        glScissor(20, 20, 2, 2);
        const QImage expected = renderer->captureFrame(paint);
        const QImage copied = renderer->captureFrame([&] {
            EXPECT_FALSE(renderer->copyOutputFrame({32, 48}));
            EXPECT_TRUE(renderer->copyOutputFrame({64, 48}));
        });
        EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
        renderer->cancelFrame();
        ASSERT_FALSE(expected.isNull());
        EXPECT_EQ(copied, expected) << "transform " << transform;
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}
