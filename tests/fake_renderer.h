// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_TESTS_FAKE_RENDERER_H_
#define CUTEFISH_TESTS_FAKE_RENDERER_H_

#include <QList>
#include <QString>
#include <QStringList>

#include <cstring>
#include <optional>
#include <vector>

#include "render/renderer.h"

namespace cutefish::testsupport {

// Keeps the pixels it was given so tests can assert what was uploaded.
class FakeTexture : public render::Texture
{
public:
    FakeTexture(const QSize &size, const uint8_t *pixels, int stride, render::PixelFormat format)
        : m_size(size)
        , m_format(format)
    {
        upload(pixels, stride);
    }

    QSize size() const override { return m_size; }

    render::PixelFormat format() const override { return m_format; }

    void upload(const uint8_t *pixels, int stride)
    {
        const int row = m_size.width() * 4;
        m_pixels.resize(static_cast<size_t>(row) * m_size.height());
        for (int y = 0; y < m_size.height(); ++y) {
            std::memcpy(m_pixels.data() + static_cast<size_t>(y) * row,
                        pixels + static_cast<size_t>(y) * stride, row);
        }
        ++uploadCount;
    }

    const std::vector<uint8_t> &pixels() const { return m_pixels; }

    int uploadCount = 0;

private:
    QSize m_size;
    render::PixelFormat m_format;
    std::vector<uint8_t> m_pixels;
};

// Records draw calls in order so paint ordering can be asserted without GL.
class FakeRenderer : public render::Renderer
{
public:
    struct Call {
        QString kind;
        QRect rect;
        render::Color color;
        render::Texture *texture = nullptr;
        float alpha = 1.0f;
        QTransform mapping;
        QRectF target;
    };

    bool beginFrame(render::RenderTarget *) override { return true; }

    bool endFrame() override { return true; }

    void clear(const render::Color &color) override
    {
        calls.append(Call {QStringLiteral("clear"), QRect(), color, nullptr, 1.0f});
    }

    void fillRect(const QRect &rect, const render::Color &color) override
    {
        calls.append(Call {QStringLiteral("fill"), rect, color, nullptr, 1.0f});
    }

    void drawTexture(render::Texture *texture, const QRect &target, float alpha) override
    {
        calls.append(Call {QStringLiteral("texture"), target, render::Color {}, texture, alpha});
    }

    void drawTextureTransformed(render::Texture *texture, const QRectF &target,
                                const QTransform &transform, float alpha) override
    {
        calls.append(Call {QStringLiteral("transformed"), target.toAlignedRect(), render::Color {},
                           texture, alpha, transform});
        calls.last().target = target;
    }

    void drawTextureMapped(render::Texture *texture, const QRect &target, float alpha,
                           const QTransform &mapping) override
    {
        drawTexture(texture, target, alpha);
        calls.last().mapping = mapping;
    }

    std::unique_ptr<render::Texture> createTexture(const QSize &size, const uint8_t *pixels,
                                                   int stride, render::PixelFormat format) override
    {
        if (size.isEmpty() || !pixels || stride < size.width() * 4 || failTextureCreation)
            return nullptr;
        ++createdTextures;
        return std::make_unique<FakeTexture>(size, pixels, stride, format);
    }

    bool updateTexture(render::Texture *texture, const uint8_t *pixels, int stride,
                       render::PixelFormat format) override
    {
        auto *fake = dynamic_cast<FakeTexture *>(texture);
        if (!fake || !pixels || fake->format() != format || stride < fake->size().width() * 4) {
            return false;
        }
        fake->upload(pixels, stride);
        return true;
    }

    // Stands in for the GPU refusing an allocation.
    bool failTextureCreation = false;

    // What the last partial update was asked to take.
    std::optional<QRegion> lastUploadDamage;

    bool updateTexture(render::Texture *texture, const uint8_t *pixels, int stride,
                       render::PixelFormat format, const QRegion &damage) override
    {
        lastUploadDamage = damage;
        return updateTexture(texture, pixels, stride, format);
    }

    using render::Renderer::createTexture;

    // A transparent image of the size asked for; the caster is not painted.
    std::unique_ptr<render::Texture> renderShadow(const QSize &size,
                                                  const std::vector<render::ShadowLobe> &lobes,
                                                  const std::function<void()> &) override
    {
        if (size.isEmpty())
            return nullptr;
        ++renderedShadows;
        lastShadowLobes = lobes;
        const std::vector<uint8_t> pixels(static_cast<size_t>(size.width()) * size.height() * 4);
        return std::make_unique<FakeTexture>(size, pixels.data(), size.width() * 4,
                                             render::PixelFormat::Rgba8888);
    }

    QSize viewportSize() const override { return QSize(1920, 1080); }

    QStringList kinds() const
    {
        QStringList result;
        for (const Call &call : calls)
            result.append(call.kind);
        return result;
    }

    QList<Call> calls;
    int createdTextures = 0;
    int renderedShadows = 0;
    std::vector<render::ShadowLobe> lastShadowLobes;
};

} // namespace cutefish::testsupport

#endif // CUTEFISH_TESTS_FAKE_RENDERER_H_
