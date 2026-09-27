// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "compositor/output.h"
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>

namespace cutefish::scene {
class Node;
}

namespace cutefish::compositor {

// What an ext_image_capture_source_v1 refers to: a whole output, or one
// window painted on its own. The owner keeps the fields current and emits
// changed() when the buffer constraints move or the source goes away.
class CaptureSource final : public QObject
{
    Q_OBJECT
public:
    enum class Kind {
        Output,
        Window,
    };

    Kind kind = Kind::Output;
    Output output;
    // Window sources: the subtree to paint, its logical bounds in global
    // coordinates, the scale it is rasterized at, and the output whose
    // composition carries it.
    const scene::Node *node = nullptr;
    QRect bounds;
    double scale = 1;
    QString outputName;
    bool active = true;

    QSize pixelSize() const
    {
        if (kind == Kind::Output)
            return output.pixelSize;
        return QSize(qRound(bounds.width() * scale), qRound(bounds.height() * scale));
    }

    QString renderedOn() const { return kind == Kind::Output ? output.name : outputName; }

Q_SIGNALS:
    void changed();
};

} // namespace cutefish::compositor
