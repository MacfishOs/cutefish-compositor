// Copyright 2012 The Chromium Authors
// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/gfx/shadow_value.cc
//           chromium/src/ui/compositor_extra/{shadow,decoration_util}.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// What is taken: the shadow geometry and cache concepts.

#ifndef CUTEFISH_DECORATION_SHADOW_H_
#define CUTEFISH_DECORATION_SHADOW_H_

#include <array>
#include <memory>

#include <QMargins>
#include <QPainterPath>
#include <QString>

#include "decoration/raster.h"

namespace cutefish::decoration {

struct ShadowLayer {
    float sigma = 0; // Gaussian standard deviation, logical px, NOT Chromium blur().
    QPoint offset; // Logical px, positive down/right.
    float opacity = 0;
};

using ShadowStyle = std::array<ShadowLayer, 3>;

// Raster geometry for a supplied silhouette; window ownership is decided by
// WindowVisualController. No surface or input geometry is changed.
class ShadowGeometry
{
public:
    // Larger corners make no shadow: the corner assets are kept small.
    static constexpr int kMaxRadius = 32;

    // A non-empty `silhouette`, window-local, replaces the rounded rectangle.
    // Its longest straight run on each axis is stretched the way the
    // nine-patch stretches the sides of a rounded rectangle.
    ShadowGeometry(const QSize &windowSize, int radius, const ShadowStyle &style,
                   int rasterScale = 1, const Corners &corners = Corners::all(),
                   const QPainterPath &silhouette = {});

    QRect paintBounds() const;

    const std::array<QRect, 9> &targets() const { return m_targets; }

    const std::array<QRect, 9> &sources() const { return m_sources; }

    QSize rasterSize() const { return m_rasterSize; }

    QSize canonicalSize() const { return m_canonicalSize; }

    int topRadius() const { return m_radius; }

    Corners corners() const { return m_corners; }

    int rasterScale() const { return m_scale; }

    QMargins margins() const { return m_margins; }

    QString key() const;
    // The nine-patch source, drawn by `renderer` at rasterSize(); null where
    // it cannot draw one.
    std::unique_ptr<render::Texture> render(render::Renderer &renderer) const;

private:
    QSize m_windowSize;
    QSize m_canonicalSize;
    QSize m_rasterSize;
    int m_radius = 0;
    int m_scale = 1;
    Corners m_corners;
    ShadowStyle m_style;
    QPainterPath m_silhouette;
    // Window-local start and end of the stretched run on each axis.
    QPoint m_stretchBegin;
    QPoint m_stretchEnd;
    QMargins m_margins;
    std::array<QRect, 9> m_targets;
    std::array<QRect, 9> m_sources;
};

void paintShadow(render::Renderer &renderer, const ShadowGeometry &geometry,
                 const QPoint &position, float opacity = 1);

} // namespace cutefish::decoration

#endif // CUTEFISH_DECORATION_SHADOW_H_
