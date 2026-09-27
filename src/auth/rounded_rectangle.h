// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_ROUNDED_RECTANGLE_H
#define CUTEFISH_AUTH_ROUNDED_RECTANGLE_H

#include <QColor>
#include <QMetaObject>
#include <QPointF>
#include <QQuickItem>

namespace cutefish::auth {

class RoundedRectangleBorder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal width READ width WRITE setWidth NOTIFY changed)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY changed)

public:
    using QObject::QObject;

    qreal width() const { return m_width; }

    void setWidth(qreal width);

    QColor color() const { return m_color; }

    void setColor(const QColor &color);

signals:
    void changed();

private:
    qreal m_width = 0;
    QColor m_color = Qt::black;
};

// Rectangle for the panel: the software renderer builds a rounded Rectangle from
// corner pieces that seam at fractional scales. One path on device pixels instead.
class RoundedRectangle : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(cutefish::auth::RoundedRectangleBorder *border READ border CONSTANT)

public:
    explicit RoundedRectangle(QQuickItem *parent = nullptr);

    QColor color() const { return m_color; }

    void setColor(const QColor &color);

    qreal radius() const { return m_radius; }

    void setRadius(qreal radius);

    RoundedRectangleBorder *border() { return &m_border; }

signals:
    void colorChanged();
    void radiusChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    QColor m_color = Qt::white;
    qreal m_radius = 0;
    RoundedRectangleBorder m_border;
    // Where the node was snapped for: a moving ancestor does not tell the item.
    QPointF m_nodeOrigin;
    QMetaObject::Connection m_frameConnection;
};

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_ROUNDED_RECTANGLE_H
