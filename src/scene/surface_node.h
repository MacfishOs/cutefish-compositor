// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SCENE_SURFACE_NODE_H_
#define CUTEFISH_SCENE_SURFACE_NODE_H_

#include <memory>
#include <optional>

#include <QPainterPath>

#include "render/glass.h"
#include "render/presentation.h"
#include "render/scanout.h"
#include "render/sync_fence.h"
#include "scene/node.h"

namespace cutefish::scene {

// The renderable content of one compositor::Surface. Unlike TextureNode it owns
// its texture, which the compositor produces from the surface's committed
// buffer.
class SurfaceNode : public Node
{
public:
    explicit SurfaceNode(const QString &name = QString());
    ~SurfaceNode() override;

    const QTransform &textureMapping() const { return m_textureMapping; }

    void setTextureMapping(const QTransform &mapping)
    {
        if (m_textureMapping == mapping)
            return;
        m_textureMapping = mapping;
        notifyChanged();
    }

    const std::shared_ptr<render::PresentationContent> &presentationContent() const
    {
        return m_presentationContent;
    }

    void setPresentationContent(std::shared_ptr<render::PresentationContent> content)
    {
        m_presentationContent = std::move(content);
    }

    render::Texture *texture() const { return m_texture.get(); }

    // Shared rather than unique because a DMA-BUF texture is imported once and
    // owned by the wl_buffer, which may outlive any one commit.
    void setTexture(std::shared_ptr<render::Texture> texture);
    std::shared_ptr<render::Texture> takeTexture();

    // `shape` is surface-local; empty for the rounded rectangle.
    void setBackgroundBlur(const QRect &rect, int radius, const render::GlassParams &glass = {},
                           const QPainterPath &shape = {});

    bool hasBackgroundBlur() const { return !m_blurRect.isEmpty(); }

    float alpha() const { return m_alpha; }

    void setAlpha(float alpha)
    {
        if (m_alpha != alpha) {
            m_alpha = alpha;
            notifyChanged();
        }
    }

    // The client storage behind the texture, when the display controller may be
    // able to scan it out instead of compositing it. Null for shm content and
    // for anything the compositor drew itself.
    const std::shared_ptr<render::ScanoutBuffer> &scanoutBuffer() const { return m_scanoutBuffer; }

    void setScanoutBuffer(std::shared_ptr<render::ScanoutBuffer> buffer);

    // Whether the content is known to be fully opaque over its whole geometry,
    // which is what lets it take the primary plane or an opaque overlay.
    bool isOpaque() const { return m_opaque && !m_contentShape; }

    void setContentShape(std::optional<QRegion> shape)
    {
        if (m_contentShape == shape)
            return;
        m_contentShape = std::move(shape);
        notifyChanged();
    }

    bool hasContentShape() const { return m_contentShape.has_value(); }

    void setOpaque(bool opaque);

    // Signalled when the client has finished writing the current buffer. Every
    // reader - the GPU for composition, KMS for scanout - waits on it first.
    const render::SyncFence &acquireFence() const { return m_acquireFence; }

    void setAcquireFence(render::SyncFence fence) { m_acquireFence = std::move(fence); }

    Content content() const override
    {
        if (!m_texture)
            return Content::None;
        return !hasBackgroundBlur() && isOpaque() && m_alpha >= 1.0f ? Content::Opaque
                                                                     : Content::Translucent;
    }

protected:
    void paint(render::Renderer &renderer, const QRect &absoluteGeometry) const override;

private:
    std::shared_ptr<render::PresentationContent> m_presentationContent;
    QTransform m_textureMapping;
    std::shared_ptr<render::Texture> m_texture;
    std::shared_ptr<render::ScanoutBuffer> m_scanoutBuffer;
    render::SyncFence m_acquireFence;
    QRect m_blurRect;
    int m_blurRadius = 0;
    QPainterPath m_blurShape;
    render::GlassParams m_glass;
    float m_alpha = 1.0f;
    bool m_opaque = false;
    std::optional<QRegion> m_contentShape;
};

} // namespace cutefish::scene

#endif // CUTEFISH_SCENE_SURFACE_NODE_H_
