// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/avatar_provider.h"

#include <QPainter>
#include <QPainterPath>

namespace cutefish::auth {

namespace {

constexpr int kDefaultSize = 192;

} // namespace

AvatarProvider::AvatarProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage AvatarProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    QImage source(id);
    if (source.isNull())
        return QImage();

    const int edge = requestedSize.width() > 0 ? requestedSize.width() : kDefaultSize;

    // Square-crop from the centre first, so a portrait or landscape picture is
    // not squashed into the circle.
    const int shortEdge = qMin(source.width(), source.height());
    source = source.copy((source.width() - shortEdge) / 2, (source.height() - shortEdge) / 2,
                         shortEdge, shortEdge)
                     .scaled(edge, edge, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QImage result(edge, edge, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath circle;
    circle.addEllipse(0, 0, edge, edge);
    painter.setClipPath(circle);
    painter.drawImage(0, 0, source);
    painter.end();

    if (size)
        *size = result.size();
    return result;
}

} // namespace cutefish::auth
