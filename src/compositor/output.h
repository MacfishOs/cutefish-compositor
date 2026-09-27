// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include "compositor/output_scale.h"
#include "render/presentation.h"

namespace cutefish::compositor {

// Metrics for the selected native mode; UI scale never changes pixelSize.
struct Output {
    enum class Subpixel {
        Unknown,
        None,
        HorizontalRgb,
        HorizontalBgr,
        VerticalRgb,
        VerticalBgr
    };

    enum class Transform {
        Normal,
        Rotate90,
        Rotate180,
        Rotate270,
    };

    QSize orientedPixelSize() const
    {
        return transform == Transform::Rotate90 || transform == Transform::Rotate270
                ? QSize(pixelSize.height(), pixelSize.width())
                : pixelSize;
    }

    QSize logicalSize() const
    {
        return logicalExtent.isEmpty() ? scale.logicalSize(orientedPixelSize()) : logicalExtent;
    }

    QPoint position;
    QSize physicalSizeMm;
    QSize pixelSize;
    QString make = QStringLiteral("unknown");
    QString model;
    int refreshMilliHz = 0;
    bool preferred = false;
    Subpixel subpixel = Subpixel::Unknown;
    OutputScale scale;
    std::shared_ptr<render::PresentationOutput> presentation =
            std::make_shared<render::PresentationOutput>();
    QString name = QStringLiteral("Cutefish-1");
    QString description;
    Transform transform = Transform::Normal;
    double renderScale = 1.0;
    QRect nativeContentRect;
    QSize logicalExtent;
    QString displayId;
};
} // namespace cutefish::compositor
