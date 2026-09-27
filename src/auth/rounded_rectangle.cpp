// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/rounded_rectangle.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGImageNode>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cutefish::auth {
namespace {

// FishUI's PixelGrid::snapped: edges move inward to whole device pixels, so every
// side meets the grid at the same phase and a border is equally heavy all round.
QRectF snapped(const QRectF &rect, const QPointF &origin, qreal scale)
{
    constexpr qreal kSlack = 1e-4;
    const auto span = [&](qreal begin, qreal end, qreal offset) {
        const qreal first = std::ceil((begin + offset) * scale - kSlack);
        const qreal last = std::max(std::floor((end + offset) * scale + kSlack), first + 1);
        return std::pair {first / scale - offset, last / scale - offset};
    };
    const auto [left, right] = span(rect.left(), rect.right(), origin.x());
    const auto [top, bottom] = span(rect.top(), rect.bottom(), origin.y());
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

} // namespace

void RoundedRectangleBorder::setWidth(qreal width)
{
    if (qFuzzyCompare(m_width + 1, width + 1))
        return;
    m_width = width;
    emit changed();
}

void RoundedRectangleBorder::setColor(const QColor &color)
{
    if (m_color == color)
        return;
    m_color = color;
    emit changed();
}

RoundedRectangle::RoundedRectangle(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    connect(&m_border, &RoundedRectangleBorder::changed, this, &QQuickItem::update);
}

void RoundedRectangle::setColor(const QColor &color)
{
    if (m_color == color)
        return;
    m_color = color;
    update();
    emit colorChanged();
}

void RoundedRectangle::setRadius(qreal radius)
{
    if (qFuzzyCompare(m_radius + 1, radius + 1))
        return;
    m_radius = radius;
    update();
    emit radiusChanged();
}

void RoundedRectangle::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    update();
}

void RoundedRectangle::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemSceneChange) {
        disconnect(m_frameConnection);
        if (value.window) {
            m_frameConnection = connect(value.window, &QQuickWindow::afterAnimating, this, [this] {
                if (isVisible() && mapToScene(QPointF()) != m_nodeOrigin)
                    update();
            });
        }
    }
    if (change == ItemDevicePixelRatioHasChanged)
        update();
    QQuickItem::itemChange(change, value);
}

QSGNode *RoundedRectangle::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    const qreal scale = window()->effectiveDevicePixelRatio();
    m_nodeOrigin = mapToScene(QPointF());
    const QRectF rect = snapped(QRectF(0, 0, width(), height()), m_nodeOrigin, scale);
    // Whole device pixels, at least one, so a hairline stays sharp at any scale.
    const qreal border =
            m_border.width() > 0 ? std::max(1.0, std::floor(m_border.width() * scale + 1e-3)) : 0;
    if (width() <= 0 || height() <= 0
        || (m_color.alpha() == 0 && (border == 0 || m_border.color().alpha() == 0))) {
        delete oldNode;
        return nullptr;
    }

    QImage image(QSize(qRound(rect.width() * scale), qRound(rect.height() * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bounds(image.rect());
    const qreal radius = std::min(m_radius * scale, std::min(bounds.width(), bounds.height()) / 2);
    QPainterPath outer;
    outer.addRoundedRect(bounds, radius, radius);
    if (border == 0) {
        painter.fillPath(outer, m_color);
    } else {
        painter.fillPath(outer, m_border.color());
        const qreal innerRadius = std::max(radius - border, 0.0);
        QPainterPath inner;
        inner.addRoundedRect(bounds.adjusted(border, border, -border, -border), innerRadius,
                             innerRadius);
        // Source blends the edge pixels by coverage: the fill meets the border without a seam.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillPath(inner, m_color);
    }
    painter.end();

    auto *node = static_cast<QSGImageNode *>(oldNode);
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
    }
    node->setTexture(window()->createTextureFromImage(image));
    node->setRect(rect);
    return node;
}

} // namespace cutefish::auth
