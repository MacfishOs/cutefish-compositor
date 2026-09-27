// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <gtest/gtest.h>
#include <stdexcept>

#include "egl_test_util.h"
#include "render/gl_program.h"

using namespace cutefish::render;

namespace {

constexpr char vertex[] =
        "attribute vec2 position; void main() { gl_Position = vec4(position, 0., 1.); }";
constexpr char fragment[] = "precision mediump float; void main() { gl_FragColor = vec4(1.); }";
constexpr char invalid[] = "this is not GLSL";

class RenderInternals : public testing::Test
{
protected:
    void SetUp() override
    {
        renderer = EglRendererTestAccess::create();
        ASSERT_TRUE(renderer);
        const EGLint attributes[] = {EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE};
        const auto surface =
                eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attributes);
        ASSERT_NE(surface, EGL_NO_SURFACE);
        target = std::make_unique<EglRenderTarget>(renderer->eglDisplay(), surface, nullptr,
                                                   QSize(32, 32));
        renderer->setOutputContext({"test", QRect(10, 20, 32, 32), {32, 32}, 1, 0, 1});
        ASSERT_TRUE(renderer->beginFrame(target.get()));
    }

    void TearDown() override
    {
        if (renderer && target)
            renderer->releaseTarget(target.get());
        target.reset();
        renderer.reset();
    }

    std::unique_ptr<EglRenderer> renderer;
    std::unique_ptr<EglRenderTarget> target;
};

TEST(ProgramInitialization, CompileLinkFailureAndMoveOwnership)
{
    auto context = EglContext::createSurfaceless();
    ASSERT_TRUE(context);
    EXPECT_FALSE(GlProgram::create(invalid, fragment));
    EXPECT_FALSE(GlProgram::create(vertex, invalid));
    EXPECT_FALSE(GlProgram::create(
            "varying vec2 value; void main() { value = vec2(1.); gl_Position = vec4(0.); }",
            "precision mediump float; varying vec3 value; void main() { gl_FragColor = vec4(value, "
            "1.); }"));
    auto program = GlProgram::create(vertex, fragment);
    ASSERT_TRUE(program);
    const GLuint name = program;
    EXPECT_GE(program.attribute("position"), 0);
    EXPECT_EQ(program.attribute("position"), glGetAttribLocation(name, "position"));
    EXPECT_EQ(program.uniform("absent"), -1);
    auto moved = std::move(program);
    EXPECT_FALSE(program);
    EXPECT_TRUE(glIsProgram(name));
    moved.reset();
    EXPECT_FALSE(glIsProgram(name));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(ProgramInitialization, PartialFailureCanRetryAndTeardownWithoutCurrentContext)
{
    auto renderer = EglRendererTestAccess::uninitialized();
    ASSERT_TRUE(renderer);
    for (int attempt = 0; attempt < 3; ++attempt) {
        EXPECT_FALSE(EglRendererTestAccess::initialize(*renderer, invalid));
        EXPECT_FALSE(EglRendererTestAccess::hasPrograms(*renderer));
    }
    ASSERT_TRUE(EglRendererTestAccess::initialize(*renderer));
    EXPECT_TRUE(EglRendererTestAccess::hasPrograms(*renderer));
    ASSERT_TRUE(
            eglMakeCurrent(renderer->eglDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    renderer.reset();
    EXPECT_EQ(eglGetCurrentContext(), EGL_NO_CONTEXT);

    renderer = EglRendererTestAccess::uninitialized();
    ASSERT_TRUE(renderer);
    EXPECT_FALSE(EglRendererTestAccess::initialize(*renderer, invalid));
    ASSERT_TRUE(
            eglMakeCurrent(renderer->eglDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    renderer.reset();
    EXPECT_EQ(eglGetCurrentContext(), EGL_NO_CONTEXT);
}

TEST(ProgramInitialization, BlurShaderFailureStillGivesARenderer)
{
    auto renderer = EglRendererTestAccess::uninitialized();
    ASSERT_TRUE(renderer);
    ASSERT_TRUE(EglRendererTestAccess::initialize(*renderer, nullptr, nullptr, invalid));
    EXPECT_FALSE(EglRendererTestAccess::hasBackdrop(*renderer));
    const EGLint attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const auto surface =
            eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attributes);
    ASSERT_NE(surface, EGL_NO_SURFACE);
    EglRenderTarget target(renderer->eglDisplay(), surface, nullptr, QSize(16, 16));
    renderer->setOutputContext({"test", QRect(0, 0, 16, 16), {16, 16}, 1, 0, 1});
    ASSERT_TRUE(renderer->beginFrame(&target));
    const auto image = renderer->captureFrame([&] {
        renderer->clear({0, 1, 0, 1});
        renderer->drawBackgroundBlur({0, 0, 16, 16}, 8, {0, 0, 16, 16}, {}, {});
    });
    renderer->releaseTarget(&target);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(0, 0), QColor(Qt::green));
}

TEST(ProgramInitialization, InvalidConfigDoesNotPreventLaterInitialization)
{
    EXPECT_FALSE(EglContext::createSurfaceless(1000));
    auto renderer = EglRendererTestAccess::create();
    ASSERT_TRUE(renderer);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, FailedReplacementPreservesDrawingAndOptionalSamplerCanFail)
{
    EXPECT_FALSE(EglRendererTestAccess::initialize(*renderer, invalid));
    EXPECT_TRUE(EglRendererTestAccess::hasPrograms(*renderer));
    auto image = renderer->captureFrame([&] {
        renderer->clear({0, 1, 0, 1});
    });
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(0, 0), QColor(Qt::green));
    ASSERT_TRUE(EglRendererTestAccess::initialize(*renderer, nullptr, invalid));
    EXPECT_FALSE(EglRendererTestAccess::hasExternalProgram(*renderer));
    const uint8_t pixel[] = {255, 0, 0, 255};
    auto texture = renderer->createTexture({1, 1}, pixel);
    ASSERT_TRUE(texture);
    image = renderer->captureFrame([&] {
        renderer->clear({0, 0, 0, 1});
        renderer->drawTexture(texture.get(), {10, 20, 32, 32}, 1);
    });
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(16, 16), QColor(Qt::red));
    EXPECT_FALSE(renderer->importDmaBuf({}));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, RenderToImageReadsUprightPixelsAndKeepsTheFrame)
{
    const uint8_t pixels[] = {255, 0, 0, 255, 0, 0, 255, 255};
    auto texture = renderer->createTexture({2, 1}, pixels);
    ASSERT_TRUE(texture);
    const auto image = renderer->renderToImage({8, 4}, [&] {
        renderer->drawTextureF(texture.get(), QRectF(0, 0, 8, 2), 1);
    });
    ASSERT_EQ(image.size(), QSize(8, 4));
    EXPECT_EQ(image.format(), QImage::Format_ARGB32_Premultiplied);
    EXPECT_EQ(image.pixelColor(1, 0), QColor(Qt::red));
    EXPECT_EQ(image.pixelColor(6, 1), QColor(Qt::blue));
    EXPECT_EQ(image.pixelColor(6, 3).alpha(), 0);
    EXPECT_EQ(renderer->outputContext().logicalViewport, QRect(10, 20, 32, 32));
    EXPECT_EQ(renderer->viewportSize(), QSize(32, 32));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, APartialUploadTakesOnlyItsDamageFromPaddedRows)
{
    // Eight pixels a row, ten apart, the way clients pad their rows.
    constexpr int stride = 10 * 4;
    std::vector<uint8_t> red(stride * 4), blue(stride * 4);
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
        blue[i + 2] = 255;
        blue[i + 3] = 255;
    }
    auto texture = renderer->createTexture({8, 4}, red.data(), stride, PixelFormat::Rgba8888);
    ASSERT_TRUE(texture);
    const QRegion damage = QRegion(QRect(2, 1, 3, 2)) + QRect(7, 3, 1, 1);
    ASSERT_TRUE(renderer->updateTexture(texture.get(), blue.data(), stride, PixelFormat::Rgba8888,
                                        damage));
    const auto image = renderer->renderToImage({8, 4}, [&] {
        renderer->drawTextureF(texture.get(), QRectF(0, 0, 8, 4), 1);
    });
    ASSERT_EQ(image.size(), QSize(8, 4));
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 8; ++x) {
            EXPECT_EQ(image.pixelColor(x, y),
                      QColor(damage.contains(QPoint(x, y)) ? Qt::blue : Qt::red))
                    << x << "," << y;
        }
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, TransformedTextureKeepsAFrameUprightAcrossOutputRotations)
{
    const uint8_t pixels[] = {255, 0, 0, 255, 0, 0, 255, 255};
    auto texture = renderer->createTexture({2, 1}, pixels);
    ASSERT_TRUE(texture);
    const QRectF viewport(10, 20, 32, 32);
    // A frame captured unrotated, turned by the difference as ScreenTransitionNode does.
    for (const auto &[transform, degrees] : {std::pair {1u, 90}, {2u, -180}, {3u, -90}}) {
        renderer->setOutputContext({"test", QRect(10, 20, 32, 32), {32, 32}, 1, transform, 1});
        QTransform turn;
        turn.translate(viewport.center().x(), viewport.center().y());
        turn.rotate(degrees);
        turn.translate(-viewport.center().x(), -viewport.center().y());
        const auto image = renderer->captureFrame([&] {
            renderer->clear({0, 0, 0, 1});
            renderer->drawTextureTransformed(texture.get(), viewport, turn, 1);
        });
        ASSERT_FALSE(image.isNull());
        EXPECT_EQ(image.pixelColor(4, 16), QColor(Qt::red)) << transform;
        EXPECT_EQ(image.pixelColor(28, 16), QColor(Qt::blue)) << transform;
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, BackdropSamplesWhatIsUnderItOnARotatedOutput)
{
    const QRect layout(0, 0, 48, 32);
    const auto paint = [&](const GlassParams &glass) {
        renderer->clear({0, 0, 0, 1});
        renderer->fillRect({0, 0, 24, 16}, {1, 0, 0, 1});
        renderer->fillRect({24, 0, 24, 16}, {0, 1, 0, 1});
        renderer->fillRect({0, 16, 24, 16}, {0, 0, 1, 1});
        renderer->fillRect({30, 20, 6, 12}, {1, 1, 1, 1});
        renderer->drawBackgroundBlur({8, 4, 28, 22}, 4, layout, glass, {});
    };
    GlassParams glass;
    glass.enabled = true;
    for (const auto &params : {GlassParams {}, glass}) {
        renderer->setOutputContext({"test", layout, layout.size(), 1, 0, 1});
        const auto upright = renderer->captureImage(renderer->outputContext(), layout.size(),
                                                    [&] { paint(params); });
        ASSERT_FALSE(upright.isNull());
        for (const uint32_t transform : {1u, 2u, 3u}) {
            const QSize physical = transform % 2 ? layout.size().transposed() : layout.size();
            renderer->setOutputContext({"test", layout, physical, 1, transform, 1});
            const auto turned = renderer->captureImage(renderer->outputContext(), physical,
                                                       [&] { paint(params); });
            ASSERT_FALSE(turned.isNull());
            int worst = 0;
            for (int y = 0; y < layout.height(); ++y) {
                for (int x = 0; x < layout.width(); ++x) {
                    const QPoint at = transform == 1
                            ? QPoint(y, layout.width() - 1 - x)
                            : transform == 2
                            ? QPoint(layout.width() - 1 - x, layout.height() - 1 - y)
                            : QPoint(layout.height() - 1 - y, x);
                    const QColor expected = upright.pixelColor(x, y);
                    const QColor actual = turned.pixelColor(at);
                    worst = std::max({worst, std::abs(expected.red() - actual.red()),
                                      std::abs(expected.green() - actual.green()),
                                      std::abs(expected.blue() - actual.blue())});
                }
            }
            EXPECT_LE(worst, 2) << "transform " << transform << " glass " << params.enabled;
        }
    }
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, CachedBackdropMasksAndScratchMatchAFreshDraw)
{
    const QRect layout(0, 0, 64, 48);
    const OutputRenderContext context {"test", layout, {80, 60}, 1.25, 0, 1};
    GlassParams glass;
    glass.enabled = true;
    const auto frame = [&](int shift) {
        return renderer->captureImage(context, {80, 60}, [&] {
            renderer->clear({0, 0, 0, 1});
            renderer->fillRect({0, 0, 32, 24}, {1, 0, 0, 1});
            renderer->fillRect({32, 24, 32, 24}, {0, 0, 1, 1});
            QPainterPath ellipse;
            const QRect lens(4 + shift, 6 + shift, 22, 16);
            ellipse.addEllipse(lens);
            renderer->drawBackgroundBlur(lens, 0, layout, {}, ellipse);
            QPainterPath triangle;
            triangle.moveTo(36, 30);
            triangle.lineTo(60, 30);
            triangle.lineTo(48, 46);
            triangle.closeSubpath();
            renderer->drawBackgroundBlur({36, 30, 24, 16}, 0, layout, glass, triangle);
        });
    };
    const auto fresh = [&](int shift) {
        renderer->releaseTarget(target.get());
        EXPECT_TRUE(renderer->beginFrame(target.get()));
        return frame(shift);
    };
    renderer->setOutputContext(context);
    const QImage still = fresh(0);
    const QImage moved = fresh(3);
    ASSERT_FALSE(still.isNull());
    ASSERT_NE(still, moved);
    for (const int shift : {0, 3, 0, 3, 0})
        EXPECT_EQ(frame(shift), shift ? moved : still) << shift;
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, APartialPassRedrawsOnlyItsRectAndKeepsTheRest)
{
    std::unique_ptr<Texture> texture;
    const QRect bounds(10, 20, 16, 8);
    ASSERT_TRUE(renderer->renderToTexture(texture, bounds, [&] {
        renderer->fillRect(bounds, {1, 0, 0, 1});
    }));
    // The top right quarter only, which also pins the bottom-up row order.
    ASSERT_TRUE(renderer->renderToTexture(texture, bounds, QRect(18, 20, 8, 4), [&] {
        renderer->fillRect(bounds, {0, 0, 1, 1});
    }));
    const auto image = renderer->captureFrame([&] {
        renderer->clear({0, 0, 0, 1});
        renderer->drawTexture(texture.get(), bounds, 1);
    });
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(12, 1), QColor(Qt::blue));
    EXPECT_EQ(image.pixelColor(12, 6), QColor(Qt::red));
    EXPECT_EQ(image.pixelColor(2, 1), QColor(Qt::red));
    EXPECT_EQ(image.pixelColor(2, 6), QColor(Qt::red));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, NestedPassAndExceptionRestoreGeometryPresentationAndGlState)
{
    PresentationSubmission submission;
    renderer->presentationSubmission = &submission;
    renderer->setRasterScale(1.25);
    const auto context = renderer->outputContext();
    auto program = GlProgram::create(vertex, fragment);
    ASSERT_TRUE(program);
    glUseProgram(program);
    glViewport(2, 3, 23, 17);
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, 5, 6, 7);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    GLuint textures[3];
    glGenTextures(3, textures);
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
    }
    float vertices[] = {0, 0, 1, 0, 0, 1};
    const GLuint attribute = program.attribute("position");
    glVertexAttribPointer(attribute, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(attribute);

    const auto check = [&] {
        EXPECT_EQ(renderer->viewportSize(), QSize(32, 32));
        EXPECT_EQ(renderer->outputContext().logicalViewport, context.logicalViewport);
        EXPECT_EQ(renderer->rasterScale(), 1.25);
        EXPECT_EQ(renderer->presentationSubmission, &submission);
        EXPECT_FALSE(renderer->capturing);
        GLint value, values[4];
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
        EXPECT_EQ(value, 0);
        glGetIntegerv(GL_VIEWPORT, values);
        EXPECT_EQ(values[0], 2);
        EXPECT_EQ(values[1], 3);
        EXPECT_EQ(values[2], 23);
        EXPECT_EQ(values[3], 17);
        glGetIntegerv(GL_SCISSOR_BOX, values);
        for (int i = 0; i < 4; ++i)
            EXPECT_EQ(values[i], 4 + i);
        EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
        EXPECT_TRUE(glIsEnabled(GL_BLEND));
        glGetIntegerv(GL_BLEND_SRC_RGB, &value);
        EXPECT_EQ(value, GL_SRC_ALPHA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &value);
        EXPECT_EQ(value, GL_ONE);
        glGetIntegerv(GL_CURRENT_PROGRAM, &value);
        EXPECT_EQ(GLuint(value), GLuint(program));
        glGetIntegerv(GL_PACK_ALIGNMENT, &value);
        EXPECT_EQ(value, 1);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &value);
        EXPECT_EQ(value, 8);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
        EXPECT_EQ(value, GL_TEXTURE2);
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &value);
            EXPECT_EQ(GLuint(value), textures[i]);
        }
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &value);
        EXPECT_TRUE(value);
        void *pointer = nullptr;
        glGetVertexAttribPointerv(attribute, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        EXPECT_EQ(pointer, vertices);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    };

    std::unique_ptr<Texture> outer, inner;
    ASSERT_TRUE(renderer->renderToTexture(outer, {11, 21, 12, 12}, [&] {
        EXPECT_EQ(renderer->presentationSubmission, nullptr);
        const QSize viewport = renderer->viewportSize();
        GLint framebuffer;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        ASSERT_TRUE(renderer->renderToTexture(inner, {12, 22, 4, 4}, [&] {
            renderer->fillRect({12, 22, 4, 4}, {0, 1, 0, 1});
        }));
        GLint restored;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &restored);
        EXPECT_EQ(restored, framebuffer);
        EXPECT_EQ(renderer->viewportSize(), viewport);
    }));
    check();
    EXPECT_THROW(renderer->renderToTexture(outer, {11, 21, 12, 12},
                                           [&] {
                                               renderer->renderToTexture(inner, {12, 22, 4, 4}, [] {
                                                   throw std::runtime_error("paint");
                                               });
                                           }),
                 std::runtime_error);
    check();
    EXPECT_THROW(renderer->captureFrame([] {
        throw std::runtime_error("capture");
    }),
                 std::runtime_error);
    check();
    glDisableVertexAttribArray(attribute);
    glUseProgram(0);
    glDeleteTextures(3, textures);
    renderer->presentationSubmission = nullptr;
}

TEST_F(RenderInternals, ReadbackRestoresEs3BuffersAndIntegerAttributes)
{
    if (!renderer->glesVersion().startsWith("OpenGL ES 3"))
        GTEST_SKIP() << "ES3 transfer-buffer state requires an ES3 context";
    const uint8_t pixel[] = {255, 0, 0, 255};
    auto texture = renderer->createTexture({1, 1}, pixel);
    ASSERT_TRUE(texture);
    GLuint readFramebuffer = 0;
    glGenFramebuffers(1, &readFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           static_cast<GlTexture *>(texture.get())->name(), 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    GLuint buffers[3];
    glGenBuffers(3, buffers);
    const GLenum targets[] = {GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, GL_ARRAY_BUFFER};
    for (int i = 0; i < 3; ++i) {
        glBindBuffer(targets[i], buffers[i]);
        glBufferData(targets[i], 4096, nullptr, GL_STREAM_DRAW);
    }
    glVertexAttribIPointer(7, 2, GL_INT, 0, nullptr);
    glEnableVertexAttribArray(7);
    auto image = renderer->captureFrame([&] {
        renderer->clear({0, 1, 0, 1});
    });
    ASSERT_EQ(image.size(), QSize(32, 32));
    EXPECT_EQ(image.pixelColor(16, 16), QColor(Qt::green));
    GLint value = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &value);
    EXPECT_EQ(GLuint(value), readFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &value);
    EXPECT_EQ(value, 0);
    const GLenum bindings[] = {GL_PIXEL_PACK_BUFFER_BINDING, GL_PIXEL_UNPACK_BUFFER_BINDING,
                               GL_ARRAY_BUFFER_BINDING};
    for (int i = 0; i < 3; ++i) {
        glGetIntegerv(bindings[i], &value);
        EXPECT_EQ(GLuint(value), buffers[i]);
        glBindBuffer(targets[i], 0);
    }
    glGetVertexAttribiv(7, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &value);
    EXPECT_TRUE(value);
    glDisableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &readFramebuffer);
    glDeleteBuffers(3, buffers);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(RenderInternals, IncompleteAndOversizedTargetsRestoreStateWithoutPainting)
{
    PresentationSubmission submission;
    renderer->presentationSubmission = &submission;
    glEnable(GL_SCISSOR_TEST);
    glScissor(1, 2, 3, 4);
    std::unique_ptr<Texture> texture = std::make_unique<GlTexture>(0, QSize(8, 8));
    bool painted = false;
    EXPECT_FALSE(renderer->renderToTexture(texture, {0, 0, 8, 8}, [&] {
        painted = true;
    }));
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    EXPECT_FALSE(renderer->renderToTexture(texture, {0, 0, limit + 1, 1}, [&] {
        painted = true;
    }));
    EXPECT_FALSE(painted);
    EXPECT_EQ(renderer->presentationSubmission, &submission);
    EXPECT_EQ(renderer->viewportSize(), QSize(32, 32));
    GLint framebuffer = -1, scissor[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    EXPECT_EQ(framebuffer, 0);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(scissor[i], i + 1);
    EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
    renderer->presentationSubmission = nullptr;
}

TEST_F(RenderInternals, TextureOutlivesRendererAndRetirementRestoresForeignContext)
{
    const uint8_t pixel[] = {255, 0, 0, 255};
    auto texture = renderer->createTexture({1, 1}, pixel);
    ASSERT_TRUE(texture);
    renderer->releaseTarget(target.get());
    target.reset();
    const EGLDisplay display = renderer->eglDisplay();
    const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLContext foreign =
            eglCreateContext(display, renderer->eglConfig(), EGL_NO_CONTEXT, attributes);
    ASSERT_NE(foreign, EGL_NO_CONTEXT);
    ASSERT_TRUE(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, foreign));
    GLuint sentinel;
    glGenTextures(1, &sentinel);
    glBindTexture(GL_TEXTURE_2D, sentinel);
    renderer.reset();
    EXPECT_EQ(eglGetCurrentContext(), foreign);
    EXPECT_TRUE(glIsTexture(sentinel));
    glDeleteTextures(1, &sentinel);
    ASSERT_TRUE(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    ASSERT_TRUE(eglDestroyContext(display, foreign));
    texture.reset();
    EXPECT_EQ(eglGetCurrentContext(), EGL_NO_CONTEXT);
}

TEST(ProgramInitialization, RetiredTextureCannotTerminateAReplacementDisplay)
{
    auto oldContext = EglContext::createSurfaceless();
    ASSERT_TRUE(oldContext);
    const uint8_t pixel[] = {255, 0, 0, 255};
    std::unique_ptr<Texture> oldTexture;
    {
        GlTextureUploader oldUploader(oldContext);
        oldTexture = oldUploader.create({1, 1}, pixel, 4, PixelFormat::Rgba8888);
    }
    ASSERT_TRUE(oldTexture);
    auto replacement = EglRendererTestAccess::create();
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->eglDisplay(), oldContext->display());
    const EGLContext current = eglGetCurrentContext();
    oldContext.reset();
    oldTexture.reset();
    EXPECT_EQ(eglGetCurrentContext(), current);
    EXPECT_FALSE(EglContext::createSurfaceless(1000));
    EXPECT_EQ(eglGetCurrentContext(), current);
    EXPECT_TRUE(GlProgram::create(vertex, fragment));
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST(EglNativeFence, AcquirePreservesDescriptorsAndReleaseRequiresAFrame)
{
    const auto path = qgetenv("CUTEFISH_TEST_DRM_DEVICE");
    if (path.isEmpty())
        GTEST_SKIP() << "Set CUTEFISH_TEST_DRM_DEVICE to test native EGL fences";
    cutefish::ScopedFd fd(open(path.constData(), O_RDWR | O_CLOEXEC));
    ASSERT_TRUE(fd.isValid());
    std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> device(gbm_create_device(fd.get()),
                                                                      gbm_device_destroy);
    ASSERT_TRUE(device);
    auto renderer = EglRenderer::create(device.get(), DRM_FORMAT_XRGB8888);
    ASSERT_TRUE(renderer);
    const QByteArray extensions(eglQueryString(renderer->eglDisplay(), EGL_EXTENSIONS));
    if (!extensions.split(' ').contains("EGL_ANDROID_native_fence_sync"))
        GTEST_SKIP() << "EGL native fences are unavailable";
    std::unique_ptr<gbm_surface, decltype(&gbm_surface_destroy)> surface(
            gbm_surface_create(device.get(), 32, 32, DRM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING),
            gbm_surface_destroy);
    ASSERT_TRUE(surface);
    auto target = renderer->createTarget(surface.get(), {32, 32});
    ASSERT_TRUE(target);
    TargetRetirement retire {renderer.get(), target.get()};
    EXPECT_FALSE(renderer->createReleaseFence().isValid());
    ASSERT_TRUE(renderer->beginFrame(target.get()));
    renderer->clear({0, 1, 0, 1});
    auto fence = renderer->createReleaseFence();
    ASSERT_TRUE(fence.isValid());
    auto duplicate = fence.duplicate();
    ASSERT_TRUE(duplicate.isValid());
    EXPECT_NE(duplicate.get(), fence.get());
    renderer->waitFence(fence);
    EXPECT_GE(fcntl(fence.get(), F_GETFD), 0);
    EXPECT_TRUE(duplicate.wait(500));
    renderer->cancelFrame();
    EXPECT_FALSE(renderer->createReleaseFence().isValid());
    renderer->waitFence(fence);
    EXPECT_GE(fcntl(fence.get(), F_GETFD), 0);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

} // namespace

TEST(EglExtensions, WholeNamesOnlyAsTheSplitListWould)
{
    auto context = EglContext::createSurfaceless();
    ASSERT_TRUE(context);
    ASSERT_TRUE(context->makeCurrent());
    const QByteArray list(reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS)));
    const QList<QByteArray> names = list.split(' ');
    ASSERT_GT(names.size(), 5);
    for (const QByteArray &name : names) {
        if (name.isEmpty())
            continue;
        EXPECT_TRUE(context->supportsGl(name.constData())) << name.constData();
        // A prefix of a name is not that name, unless it is one itself.
        const QByteArray prefix = name.left(name.size() - 1);
        EXPECT_EQ(names.contains(prefix), context->supportsGl(prefix.constData()))
                << prefix.constData();
    }
    EXPECT_FALSE(context->supportsGl(""));
    EXPECT_FALSE(context->supportsGl("GL_CUTEFISH_not_an_extension"));
}
