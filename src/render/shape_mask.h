// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QImage>
#include <QPainterPath>
#include <QRect>
#include <QString>

#include "render/renderer.h"

namespace cutefish::render {

// Identity of a path for texture cache keys.
QString shapeKey(const QPainterPath &path);

// A logical path placed on physical pixels: pixel = point * scale + offset.
struct ShapeRaster {
    double scale = 1;
    QPointF offset;
    QSize size;

    // Covers `logical` with `margin` physical pixels around it, keeping the
    // image origin on a physical pixel of the output.
    static ShapeRaster covering(const QRectF &logical, double scale, int margin = 0);

    // Where the image goes, in logical output coordinates.
    QRectF target() const;

    QString key() const;

    bool operator==(const ShapeRaster &) const = default;
};

// Premultiplied white, so the alpha is the coverage.
QImage rasterizeShape(const QPainterPath &path, const ShapeRaster &raster);

// A hairline of `widthPx` physical pixels just outside the path in `outline`
// and just inside it in `highlight`.
QImage rasterizeShapeOutline(const QPainterPath &path, const ShapeRaster &raster, qreal widthPx,
                             const Color &outline, const Color &highlight);

} // namespace cutefish::render
