// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>

#include <QRectF>

namespace cutefish::render {

// The session's corner style. Every window silhouette follows it, so it is
// one flag consulted where shapes are drawn or hit rather than a field on
// each shape; decoration::ThemePreferences keeps it in step with the
// Appearance setting.
bool continuousCorners();
void setContinuousCorners(bool continuous);

// Cubic superellipse corners meet straight edges with zero curvature.
// The 1.5 extent preserves roughly the circular radius's diagonal inset.
inline qreal continuousCornerExtent(qreal radius, qreal halfSize)
{
    // Keep the extent and exponent in sync with shaders/rounded_clip.glsl.
    return std::clamp(radius * 1.5, qreal(0), std::max(halfSize, qreal(0)));
}

// The corner's reach along one axis in the session's style.
inline qreal cornerExtent(qreal radius, qreal halfSize)
{
    return continuousCorners() ? continuousCornerExtent(radius, halfSize)
                               : std::clamp(radius, qreal(0), std::max(halfSize, qreal(0)));
}

// One output-coordinate shape shared by every surface in a window tree.
struct RoundedShape {
    QRectF bounds;
    qreal radius = 0;
    QSizeF radiusScale {1, 1};

    bool contains(const QPointF& point) const
    {
        if (!bounds.contains(point))
            return false;
        const qreal rx = cornerExtent(radius * radiusScale.width(), bounds.width() / 2);
        const qreal ry = cornerExtent(radius * radiusScale.height(), bounds.height() / 2);
        if (rx <= 0 || ry <= 0)
            return true;
        const QPointF d = point - bounds.center();
        const qreal x = std::max(std::abs(d.x()) - bounds.width() / 2 + rx, qreal(0)) / rx;
        const qreal y = std::max(std::abs(d.y()) - bounds.height() / 2 + ry, qreal(0)) / ry;
        if (!continuousCorners())
            return x * x + y * y <= 1;
        return x * x * x + y * y * y <= 1;
    }

    // Signed distance to the outline, negative inside; roundedDistance() in
    // shaders/rounded_clip.glsl.
    qreal distance(const QPointF &point) const
    {
        const QPointF half(bounds.width() / 2, bounds.height() / 2);
        const qreal rx = cornerExtent(radius * radiusScale.width(), half.x());
        const qreal ry = cornerExtent(radius * radiusScale.height(), half.y());
        const QPointF d = point - bounds.center();
        const qreal ex = std::abs(d.x()) - half.x();
        const qreal ey = std::abs(d.y()) - half.y();
        if (rx <= 0 || ry <= 0 || ex + rx <= 0 || ey + ry <= 0)
            return std::max(ex, ey);
        const qreal qx = (ex + rx) / rx;
        const qreal qy = (ey + ry) / ry;
        if (!continuousCorners()) {
            const qreal l = std::hypot(qx, qy);
            return (l - 1) * l / std::hypot(qx / rx, qy / ry);
        }
        const qreal l = std::cbrt(qx * qx * qx + qy * qy * qy);
        return (l - 1) * l * l / std::hypot(qx * qx / rx, qy * qy / ry);
    }
};

} // namespace cutefish::render
