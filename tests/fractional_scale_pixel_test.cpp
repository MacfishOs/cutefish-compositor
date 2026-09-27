// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <GLES3/gl3.h>
#include <gtest/gtest.h>

#include "compositor/compositor.h"
#include "compositor/shm_pool.h"
#include "compositor/surface.h"
#include "egl_test_util.h"
#include "scene/scene.h"
#include "shm_test_util.h"

using namespace cutefish;

namespace {

class FractionalScalePixelTest : public testing::Test
{
protected:
    static constexpr int width = 128;
    static constexpr int height = 128;
    std::unique_ptr<render::EglRenderer> renderer;
    std::unique_ptr<render::EglRenderTarget> target;

    virtual int alphaSize() const { return 8; }

    void SetUp() override
    {
        renderer = render::EglRendererTestAccess::create(alphaSize());
        ASSERT_TRUE(renderer);
        const EGLint attributes[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        const EGLSurface surface =
                eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attributes);
        ASSERT_NE(surface, EGL_NO_SURFACE);
        target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), surface, nullptr,
                                                           QSize(width, height));
        ASSERT_TRUE(renderer->beginFrame(target.get()));
    }

    void TearDown() override
    {
        if (renderer) {
            renderer->clearTextureCache();
            renderer->releaseTarget(target.get());
        }
        target.reset();
        renderer.reset();
    }

    std::vector<uint8_t> pixels()
    {
        std::vector<uint8_t> result(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
        EXPECT_EQ(glGetError(), GLenum(GL_NO_ERROR));
        return result;
    }

    int channel(const std::vector<uint8_t> &image, int x, int y, int c = 0)
    {
        return image[((height - 1 - y) * width + x) * 4 + c];
    }
};

TEST_F(FractionalScalePixelTest, NativeDensityPixelsStaySharpAtFractionalOrigins)
{
    for (double scale : {1.25, 1.5, 1.75, 2.0}) {
        renderer->setRasterScale(scale);
        const QSize logical(33, 25);
        const QSize physical(qRound(logical.width() * scale), qRound(logical.height() * scale));
        std::vector<uint8_t> storage(physical.width() * physical.height() * 4);
        for (int y = 0; y < physical.height(); ++y) {
            for (int x = 0; x < physical.width(); ++x) {
                const int offset = (y * physical.width() + x) * 4;
                storage[offset] = x % 2 ? 255 : 0;
                storage[offset + 1] = y % 2 ? 255 : 0;
                storage[offset + 2] = 73;
                storage[offset + 3] = 255;
            }
        }
        const int fd = testsupport::makeMemfd(storage.size());
        ASSERT_GE(fd, 0);
        ASSERT_EQ(pwrite(fd, storage.data(), storage.size(), 0), ssize_t(storage.size()));
        auto pool = compositor::ShmPool::create(fd, storage.size());
        auto buffer = pool->createBuffer(physical, 0, physical.width() * 4,
                                         render::PixelFormat::Rgba8888, nullptr);
        scene::Scene scene;
        compositor::Compositor compositor(scene.root());
        auto surface = compositor.createSurface();
        surface->setViewport(logical);
        surface->attach(buffer, {});
        surface->commit();
        surface->updateTexture(*renderer);
        for (QPoint outputOrigin : {QPoint(), QPoint(200, 100), QPoint(-200, -100)}) {
            renderer->setOutputContext({"test",
                                        QRect(outputOrigin, QSize(128, 128)),
                                        QSize(width, height),
                                        scale,
                                        0,
                                        1,
                                        {}});
            for (QPoint localPosition : {QPoint(3, 5), QPoint(6, 7), QPoint(9, 10)}) {
                const QPoint position = outputOrigin + localPosition;
                surface->setPosition(position);
                for (bool snapshot : {false, true}) {
                    renderer->clear({0, 0, 0, 0});
                    if (snapshot) {
                        std::unique_ptr<render::Texture> texture;
                        const QRect coverage(position - QPoint(2, 2), logical + QSize(4, 4));
                        ASSERT_TRUE(renderer->renderToTexture(texture, coverage, [&] {
                            scene.root()->render(*renderer);
                        }));
                        renderer->drawTexture(texture.get(), coverage, 1);
                    } else {
                        scene.root()->render(*renderer);
                    }
                    const auto image = pixels();
                    const QPoint origin(qRound(localPosition.x() * scale),
                                        qRound(localPosition.y() * scale));
                    for (int y = 0; y < physical.height(); ++y) {
                        for (int x = 0; x < physical.width(); ++x) {
                            const int offset = (y * physical.width() + x) * 4;
                            for (int c = 0; c < 4; ++c)
                                ASSERT_NEAR(channel(image, origin.x() + x, origin.y() + y, c),
                                            storage[offset + c], 1)
                                        << "scale=" << scale << " snapshot=" << snapshot
                                        << " pixel=" << x << "," << y << " channel=" << c;
                        }
                    }
                }
            }
        }
    }
}

// Xwayland sizes popups in device pixels. 22 pixels at 1.25 are 17.6 logical
// units, drawn as 18 (22.5 pixels): every texel must still land on a pixel.
TEST_F(FractionalScalePixelTest, ClientScaledBuffersAPixelOffStaySharp)
{
    const struct {
        double scale;
        QSize buffer;
    } cases[] = {{1.25, {22, 27}}, {1.5, {22, 25}}, {1.75, {20, 27}}, {1.75, {22, 24}},
                 {2.0, {21, 23}}};
    for (const auto &c : cases) {
        renderer->setRasterScale(c.scale);
        renderer->setOutputContext({"test", QRect(QPoint(), QSize(128, 128)), QSize(width, height),
                                    c.scale, 0, 1, {}});
        const QSize physical = c.buffer;
        std::vector<uint8_t> storage(physical.width() * physical.height() * 4);
        for (int y = 0; y < physical.height(); ++y) {
            for (int x = 0; x < physical.width(); ++x) {
                const int offset = (y * physical.width() + x) * 4;
                storage[offset] = x % 2 ? 255 : 0;
                storage[offset + 1] = y % 2 ? 255 : 0;
                storage[offset + 2] = 73;
                storage[offset + 3] = 255;
            }
        }
        const int fd = testsupport::makeMemfd(storage.size());
        ASSERT_GE(fd, 0);
        ASSERT_EQ(pwrite(fd, storage.data(), storage.size(), 0), ssize_t(storage.size()));
        auto pool = compositor::ShmPool::create(fd, storage.size());
        auto buffer = pool->createBuffer(physical, 0, physical.width() * 4,
                                         render::PixelFormat::Rgba8888, nullptr);
        scene::Scene scene;
        compositor::Compositor compositor(scene.root());
        auto surface = compositor.createSurface();
        surface->setClientScale(c.scale);
        surface->attach(buffer, {});
        surface->commit();
        surface->updateTexture(*renderer);
        const QSize device(qRound(surface->contentSize().width() * c.scale),
                           qRound(surface->contentSize().height() * c.scale));
        ASSERT_NE(device, physical) << "scale=" << c.scale;
        const QPoint localPosition(9, 10);
        surface->setPosition(localPosition);
        renderer->clear({0, 0, 0, 0});
        scene.root()->render(*renderer);
        const auto image = pixels();
        const QPoint origin(qRound(localPosition.x() * c.scale), qRound(localPosition.y() * c.scale));
        const QSize checked = physical.boundedTo(device);
        for (int y = 0; y < checked.height(); ++y) {
            for (int x = 0; x < checked.width(); ++x) {
                const int offset = (y * physical.width() + x) * 4;
                for (int channelIndex = 0; channelIndex < 4; ++channelIndex)
                    ASSERT_NEAR(channel(image, origin.x() + x, origin.y() + y, channelIndex),
                                storage[offset + channelIndex], 1)
                            << "scale=" << c.scale << " pixel=" << x << "," << y;
            }
        }
    }
}

} // namespace
