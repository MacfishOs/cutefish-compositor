// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <vector>

#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QString>

#include "input/cursor_shape.h"

namespace cutefish::cursor {

struct Frame {
    QImage image;
    QPoint hotspot;
    int delayMs = 0;
};

QString resourceName(input::CursorShape shape);
std::vector<Frame> readXCursor(const QByteArray &bytes, int size);
std::vector<Frame> load(input::CursorShape shape, int size,
                        const QString &theme = QStringLiteral("default"));
QStringList themePaths();
bool validThemeName(const QString &theme);
int defaultSize();

} // namespace cutefish::cursor
