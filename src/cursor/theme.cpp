// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cursor/theme.h"

#include <algorithm>
#include <array>
#include <limits>

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

namespace cutefish::cursor {

QString resourceName(input::CursorShape shape)
{
    static constexpr std::array names = {"",
                                         "default",
                                         "context-menu",
                                         "help",
                                         "pointer",
                                         "progress",
                                         "wait",
                                         "cell",
                                         "crosshair",
                                         "text",
                                         "vertical-text",
                                         "alias",
                                         "copy",
                                         "move",
                                         "no-drop",
                                         "not-allowed",
                                         "grab",
                                         "grabbing",
                                         "e-resize",
                                         "n-resize",
                                         "ne-resize",
                                         "nw-resize",
                                         "s-resize",
                                         "se-resize",
                                         "sw-resize",
                                         "w-resize",
                                         "ew-resize",
                                         "ns-resize",
                                         "nesw-resize",
                                         "nwse-resize",
                                         "col-resize",
                                         "row-resize",
                                         "all-scroll",
                                         "zoom-in",
                                         "zoom-out"};
    const auto index = static_cast<size_t>(shape);
    return QString::fromLatin1(names[index < names.size() ? index : 1]);
}

std::vector<Frame> readXCursor(const QByteArray &bytes, int size)
{
    QDataStream in(bytes);
    in.setByteOrder(QDataStream::LittleEndian);
    quint32 magic, header, version, count;
    in >> magic >> header >> version >> count;
    if (in.status() != QDataStream::Ok || magic != 0x72756358 || header < 16 || count > 4096
        || quint64(header) + quint64(count) * 12 > quint64(bytes.size()))
        return {};
    in.device()->seek(header);

    struct Entry {
        quint32 size;
        quint32 offset;
    };

    std::vector<Entry> entries;
    quint32 bestSize = 0;
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    for (quint32 i = 0; i < count; ++i) {
        quint32 type, nominal, offset;
        in >> type >> nominal >> offset;
        if (type != 0xfffd0002)
            continue;
        entries.push_back({nominal, offset});
        const auto distance = std::abs(qint64(nominal) - size);
        if (distance < bestDistance || (distance == bestDistance && nominal > bestSize)) {
            bestDistance = distance;
            bestSize = nominal;
        }
    }
    std::vector<Frame> result;
    quint64 totalPixels = 0;
    for (const auto &entry : entries) {
        if (entry.size != bestSize)
            continue;
        if (quint64(entry.offset) + 36 > quint64(bytes.size()))
            return {};
        in.device()->seek(entry.offset);
        quint32 chunkHeader, type, nominal, chunkVersion, width, height, xhot, yhot, delay;
        in >> chunkHeader >> type >> nominal >> chunkVersion >> width >> height >> xhot >> yhot
                >> delay;
        totalPixels += quint64(width) * height;
        if (chunkHeader < 36 || type != 0xfffd0002 || nominal != bestSize || !width || !height
            || width > 256 || height > 256 || xhot >= width || yhot >= height
            || totalPixels > 16 * 1024 * 1024
            || quint64(entry.offset) + chunkHeader + quint64(width) * height * 4
                    > quint64(bytes.size()))
            return {};
        in.device()->seek(quint64(entry.offset) + chunkHeader);
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            return {};
        for (quint32 y = 0; y < height; ++y) {
            auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
            for (quint32 x = 0; x < width; ++x)
                in >> row[x];
        }
        if (in.status() != QDataStream::Ok)
            return {};
        result.push_back(
                {std::move(image), QPoint(xhot, yhot), int(std::clamp(delay, 1u, 60000u))});
    }
    return result;
}

bool validThemeName(const QString &theme)
{
    return !theme.isEmpty() && theme.size() <= 255 && theme != "." && theme != ".."
            && !theme.contains('/') && !theme.contains('\\') && !theme.contains(QChar::Null);
}

QStringList themePaths()
{
    QStringList paths = qEnvironmentVariable("XCURSOR_PATH").split(':', Qt::SkipEmptyParts);
    if (paths.isEmpty()) {
        paths.append(QDir::homePath() + QStringLiteral("/.icons"));
        for (const auto &path :
             QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
            paths.append(path + QStringLiteral("/icons"));
        paths.append(QStringLiteral("/usr/share/pixmaps"));
    }
    for (auto &path : paths) {
        if (path.startsWith(QStringLiteral("~/")))
            path.replace(0, 1, QDir::homePath());
    }

    paths.removeDuplicates();
    return paths;
}

std::vector<Frame> load(input::CursorShape shape, int size, const QString &theme)
{
    if (shape == input::CursorShape::Hidden)
        return {};
    const auto paths = themePaths();
    QStringList themes;
    QSet<QString> visited;
    const auto collect = [&](auto &&self, const QString &name) -> void {
        if (!validThemeName(name) || visited.contains(name) || visited.size() >= 64)
            return;
        visited.insert(name);
        themes.append(name);
        for (const auto &path : paths) {
            QSettings index(QDir(path).filePath(name + "/index.theme"), QSettings::IniFormat);
            const auto parents = index.value("Icon Theme/Inherits").toStringList();
            for (const auto &parent : parents)
                self(self, parent.trimmed());
        }
    };
    collect(collect, theme);
    collect(collect, QStringLiteral("default"));
    collect(collect, QStringLiteral("Adwaita"));
    QStringList names {resourceName(shape)};
    static const QHash<QString, QStringList> aliases {
            {"default", {"left_ptr", "arrow"}},
            {"pointer", {"hand2", "hand1"}},
            {"text", {"xterm"}},
            {"wait", {"watch"}},
            {"progress", {"left_ptr_watch"}},
            {"move", {"fleur"}},
            {"ew-resize", {"sb_h_double_arrow", "size_hor"}},
            {"ns-resize", {"sb_v_double_arrow", "size_ver"}},
            {"nesw-resize", {"fd_double_arrow", "size_bdiag"}},
            {"nwse-resize", {"bd_double_arrow", "size_fdiag"}},
            {"not-allowed", {"crossed_circle"}},
            {"help", {"question_arrow"}},
            {"n-resize", {"top_side"}},
            {"s-resize", {"bottom_side"}},
            {"e-resize", {"right_side"}},
            {"w-resize", {"left_side"}},
            {"ne-resize", {"top_right_corner"}},
            {"nw-resize", {"top_left_corner"}},
            {"se-resize", {"bottom_right_corner"}},
            {"sw-resize", {"bottom_left_corner"}},
            {"grab", {"openhand"}},
            {"grabbing", {"closedhand"}}};
    names.append(aliases.value(names.front()));
    const auto find = [&](const QStringList &candidates) -> std::vector<Frame> {
        for (const auto &themeName : themes) {
            for (const auto &name : candidates) {
                for (const auto &path : paths) {
                    QFile file(QDir(path).filePath(themeName + "/cursors/" + name));
                    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
                        continue;
                    auto frames = readXCursor(file.readAll(), size);
                    if (!frames.empty())
                        return frames;
                }
            }
        }
        return {};
    };
    auto frames = find(names);
    if (frames.empty())
        frames = find({QStringLiteral("default"), QStringLiteral("left_ptr")});
    if (!frames.empty())
        return frames;
    // Keep the pointer visible even when no usable cursor theme is installed.
    const int pixels = std::clamp(size, 16, 256);
    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.scale(pixels / 24.0, pixels / 24.0);
        painter.setPen(QPen(Qt::white, 1));
        painter.setBrush(Qt::black);
        painter.drawPolygon(
                QPolygonF {{1, 1}, {1, 20}, {6, 15}, {10, 23}, {13, 21}, {9, 14}, {17, 14}});
    }
    return {{image, QPoint(pixels / 24, pixels / 24), 0}};
}

int defaultSize()
{
    bool ok = false;
    const int size = qEnvironmentVariableIntValue("XCURSOR_SIZE", &ok);
    return ok && size >= 16 && size <= 64 ? size : 24;
}

} // namespace cutefish::cursor
