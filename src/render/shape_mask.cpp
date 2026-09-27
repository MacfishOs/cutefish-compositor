// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/shape_mask.h"

#include <cmath>

#include <QCryptographicHash>
#include <QPainter>
#include <QPainterPathStroker>

namespace cutefish::render {

namespace {

QTransform deviceTransform(const ShapeRaster &raster)
{
    QTransform transform;
    transform.translate(raster.offset.x(), raster.offset.y());
    transform.scale(raster.scale, raster.scale);
    return transform;
}

QImage transparentImage(const QSize &size)
{
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

QColor toQColor(const Color &color)
{
    return QColor::fromRgbF(color.r, color.g, color.b, color.a);
}

} // namespace

QString shapeKey(const QPainterPath &path)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        const double values[] = {double(element.type), element.x, element.y};
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(values), sizeof(values)));
    }
    hash.addData(QByteArrayView(path.fillRule() == Qt::WindingFill ? "w" : "o"));
    return QString::fromLatin1(hash.result().toHex());
}

ShapeRaster ShapeRaster::covering(const QRectF &logical, double scale, int margin)
{
    const QPoint first(int(std::floor(logical.left() * scale)) - margin,
                       int(std::floor(logical.top() * scale)) - margin);
    const QPoint last(int(std::ceil(logical.right() * scale)) + margin,
                      int(std::ceil(logical.bottom() * scale)) + margin);
    return {scale, -QPointF(first), QSize(last.x() - first.x(), last.y() - first.y())};
}

QRectF ShapeRaster::target() const
{
    return QRectF(-offset / scale, QSizeF(size) / scale);
}

QString ShapeRaster::key() const
{
    return QStringLiteral("%1:%2x%3:%4,%5")
            .arg(scale, 0, 'g', 9)
            .arg(size.width())
            .arg(size.height())
            .arg(offset.x(), 0, 'g', 9)
            .arg(offset.y(), 0, 'g', 9);
}

QImage rasterizeShape(const QPainterPath &path, const ShapeRaster &raster)
{
    QImage image = transparentImage(raster.size);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(deviceTransform(raster));
    painter.fillPath(path, Qt::white);
    return image;
}

QImage rasterizeShapeOutline(const QPainterPath &path, const ShapeRaster &raster, qreal widthPx,
                             const Color &outline, const Color &highlight)
{
    const QTransform transform = deviceTransform(raster);
    // Stroked in physical pixels, so the hairline stays one pixel at any scale.
    const QPainterPath physical = transform.map(path);
    QPainterPathStroker stroker;
    stroker.setWidth(2 * widthPx);
    stroker.setJoinStyle(Qt::RoundJoin);
    const QPainterPath band = stroker.createStroke(physical);

    const auto ring = [&](const Color &color, QPainter::CompositionMode cut) {
        QImage image = transparentImage(raster.size);
        if (color.a <= 0)
            return image;
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(band, toQColor(color));
        painter.setCompositionMode(cut);
        painter.fillPath(physical, Qt::white);
        return image;
    };

    QImage image = ring(outline, QPainter::CompositionMode_DestinationOut);
    const QImage inner = ring(highlight, QPainter::CompositionMode_DestinationIn);
    QPainter painter(&image);
    painter.drawImage(0, 0, inner);
    return image;
}

} // namespace cutefish::render
