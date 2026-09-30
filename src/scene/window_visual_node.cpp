// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "scene/window_visual_node.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "render/shape_mask.h"

namespace cutefish::scene {

namespace {

// ash/wm/window_dimmer.cc kDefaultDimAnimationDurationMs.
constexpr qint64 kDimDurationMs = 200;

} // namespace

void paintWindowOutline(render::Renderer &renderer, const render::RoundedShape &shape,
                        const WindowVisualState &state, float opacity)
{
    const qreal stroke = state.outlineWidthPx / renderer.rasterScale();
    auto outer = shape;
    outer.bounds.adjust(-stroke, -stroke, stroke, stroke);
    if (shape.radius > 0)
        outer.radius += stroke / std::max(shape.radiusScale.width(), shape.radiusScale.height());
    auto outline = state.outlineColor;
    outline.a *= opacity;
    renderer.drawRoundedOutline(outer, outline, state.outlineWidthPx);
    if (state.highlightColor.a <= 0)
        return;
    auto highlight = state.highlightColor;
    highlight.a *= opacity;
    renderer.drawRoundedOutline(shape, highlight, state.outlineWidthPx);
}

void paintShapeOutline(render::Renderer &renderer, const WindowVisualState &state,
                       const QTransform &transform, float opacity)
{
    const auto raster =
            render::ShapeRaster::covering(state.outlinePath.boundingRect(), renderer.rasterScale(),
                                          int(std::ceil(state.outlineWidthPx)) + 1);
    const auto color = [](const render::Color &c) {
        return QStringLiteral(":%1,%2,%3,%4").arg(c.r).arg(c.g).arg(c.b).arg(c.a);
    };
    const QString key = QStringLiteral("shape-outline:") + render::shapeKey(state.outlinePath)
            + raster.key() + color(state.outlineColor) + color(state.highlightColor)
            + QString::number(state.outlineWidthPx);
    const auto &textures = renderer.cachedTextureSet(key, [&] {
        const QImage image =
                render::rasterizeShapeOutline(state.outlinePath, raster, state.outlineWidthPx,
                                              state.outlineColor, state.highlightColor);
        render::Renderer::TextureSet set;
        set.push_back(renderer.createTexture(image.size(), image.constBits(), image.bytesPerLine(),
                                             render::PixelFormat::Rgba8888));
        return set;
    });
    if (textures.front())
        renderer.drawTextureF(textures.front().get(), transform.mapRect(raster.target()), opacity);
}

QPointF clientBodySnap(const render::Renderer &renderer, const WindowVisualState &state,
                       const QPointF &position)
{
    if (state.outlineShape.bounds.isEmpty())
        return {};
    const QPointF surface = position + state.outlineSurfaceOrigin;
    return renderer.pixelAligned(surface) - surface;
}

float WindowVisualNode::presentedDim(const render::Renderer &renderer) const
{
    if (!m_dimFading)
        return m_dimPresented;
    // Outside an output frame there is nothing to pace the fade against.
    if (renderer.frameTime < 0) {
        m_dimFading = false;
        return m_dimPresented = m_state.dimOpacity;
    }
    if (m_dimStarted < 0)
        m_dimStarted = renderer.frameTime;
    const double progress =
            std::clamp((renderer.frameTime - m_dimStarted) / kDimDurationMs, 0.0, 1.0);
    m_dimPresented = m_dimFrom + (m_state.dimOpacity - m_dimFrom) * float(progress);
    if (progress >= 1)
        m_dimFading = false;
    else
        notifyChanged();
    return m_dimPresented;
}

void WindowVisualNode::render(render::Renderer &renderer) const
{
    if (!isVisible())
        return;
    auto shape = m_state.shape;
    shape.bounds.translate(absolutePosition());
    // Taken here, so a window nested below keeps its own corners.
    const auto corners = std::exchange(renderer.windowCorners, std::nullopt);
    if (corners) {
        shape.radius = corners->radius;
        shape.radiusScale = corners->radiusScale;
    }
    const bool clip = m_state.clipContent || corners;
    if (clip)
        renderer.pushRoundedClip(shape);
    Node::render(renderer);
    if (const float dim = presentedDim(renderer); dim > 0)
        renderer.fillRectF(shape.bounds, {0, 0, 0, dim});
    if (clip)
        renderer.popRoundedClip();
    if (m_state.drawOutline && !renderer.suppressWindowOutlines && !m_state.outlinePath.isEmpty()) {
        // Rasterized where it lands, so it stays on physical pixels at any scale.
        auto state = m_state;
        const QPointF position = absolutePosition();
        state.outlinePath.translate(position + clientBodySnap(renderer, m_state, position));
        paintShapeOutline(renderer, state, QTransform());
    } else if (m_state.drawOutline && !renderer.suppressWindowOutlines) {
        const QPointF position = absolutePosition();
        auto outline = m_state.outlined();
        outline.bounds.translate(position + clientBodySnap(renderer, m_state, position));
        paintWindowOutline(renderer, outline, m_state);
    }
}

} // namespace cutefish::scene
