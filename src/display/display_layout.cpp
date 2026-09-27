// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/display_layout.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: resolve a bounded Qt layout graph, including primary
// rerooting and minimum shared-edge normalization, without Ash display state.

#include "display/display_layout.h"

#include <algorithm>
#include <cstdlib>

#include <QQueue>
#include <QSet>

namespace cutefish::display {

namespace {

DisplayLayout::Result failure(QString message, QList<DisplayId> displays = {})
{
    DisplayLayout::Result result;
    result.error = {DisplayErrorCode::InvalidLayout, std::move(message), std::move(displays)};
    return result;
}

LayoutEdge opposite(LayoutEdge edge)
{
    switch (edge) {
    case LayoutEdge::Top:
        return LayoutEdge::Bottom;
    case LayoutEdge::Right:
        return LayoutEdge::Left;
    case LayoutEdge::Bottom:
        return LayoutEdge::Top;
    case LayoutEdge::Left:
        return LayoutEdge::Right;
    }
    return LayoutEdge::Right;
}

int normalizedOffset(int offset, LayoutEdge edge, const QSize& child, const QSize& parent,
                     int minimumSharedEdge)
{
    const bool horizontalEdge = edge == LayoutEdge::Top || edge == LayoutEdge::Bottom;
    const int childExtent = horizontalEdge ? child.width() : child.height();
    const int parentExtent = horizontalEdge ? parent.width() : parent.height();
    const int shared = std::min({minimumSharedEdge, childExtent,
                                 parentExtent});
    return std::clamp(offset, -childExtent + shared, parentExtent - shared);
}

QPoint childOrigin(const QRect& parent, const QSize& childSize, LayoutEdge edge, int offset)
{
    switch (edge) {
    case LayoutEdge::Top:
        return QPoint(parent.x() + offset, parent.y() - childSize.height());
    case LayoutEdge::Right:
        return QPoint(parent.x() + parent.width(), parent.y() + offset);
    case LayoutEdge::Bottom:
        return QPoint(parent.x() + offset, parent.y() + parent.height());
    case LayoutEdge::Left:
        return QPoint(parent.x() - childSize.width(), parent.y() + offset);
    }
    return parent.topLeft();
}

} // namespace

DisplayLayout::Result DisplayLayout::resolve(const DisplayId& primary,
                                             const QHash<DisplayId, QSize>& displaySizes,
                                             const QList<DisplayPlacement>& placements,
                                             int minimumSharedEdge)
{
    if (minimumSharedEdge < 0)
        return failure(QStringLiteral("The minimum shared edge cannot be negative"));
    if (displaySizes.isEmpty())
        return failure(QStringLiteral("At least one enabled display is required"));
    if (displaySizes.size() > maximumDisplayCount)
        return failure(QStringLiteral("The layout contains too many displays"));
    if (!primary.isValid() || !displaySizes.contains(primary))
        return failure(QStringLiteral("The primary display is missing"), {primary});

    for (auto it = displaySizes.cbegin(); it != displaySizes.cend(); ++it) {
        if (!it.key().isValid() || it.value().width() <= 0 || it.value().height() <= 0)
            return failure(QStringLiteral("A display has an invalid identity or size"), {it.key()});
    }
    if (placements.size() != displaySizes.size() - 1)
        return failure(QStringLiteral("The placement graph must contain one edge per non-primary display"));

    QList<DisplayPlacement> normalized;
    QHash<DisplayId, QList<int>> adjacency;
    normalized.reserve(placements.size());
    for (const DisplayPlacement& requested : placements) {
        if (!displaySizes.contains(requested.display) || !displaySizes.contains(requested.parent))
            return failure(QStringLiteral("A placement refers to an unknown display"),
                           {requested.display, requested.parent});
        if (requested.display == requested.parent) {
            return failure(QStringLiteral("A display cannot be placed relative to itself"),
                           {requested.display});
        }
        if (std::abs(int64_t(requested.offset)) > maximumCoordinate)
            return failure(QStringLiteral("A placement offset is outside the supported range"),
                           {requested.display});

        DisplayPlacement placement = requested;
        placement.offset = normalizedOffset(placement.offset, placement.edge,
                                            displaySizes.value(placement.display),
                                            displaySizes.value(placement.parent), minimumSharedEdge);
        const int index = normalized.size();
        normalized.append(placement);
        adjacency[placement.display].append(index);
        adjacency[placement.parent].append(index);
    }

    Result result;
    result.bounds.insert(primary, QRect(QPoint(), displaySizes.value(primary)));
    QQueue<DisplayId> pending;
    pending.enqueue(primary);
    while (!pending.isEmpty()) {
        const DisplayId current = pending.dequeue();
        for (const int index : adjacency.value(current)) {
            const DisplayPlacement& edge = normalized.at(index);
            const bool forward = edge.parent == current;
            const DisplayId next = forward ? edge.display : edge.parent;
            if (result.bounds.contains(next))
                continue;

            const LayoutEdge direction = forward ? edge.edge : opposite(edge.edge);
            const int offset = forward ? edge.offset : -edge.offset;
            const QSize nextSize = displaySizes.value(next);
            result.bounds.insert(next,
                                 QRect(childOrigin(result.bounds.value(current), nextSize,
                                                   direction, offset),
                                       nextSize));
            pending.enqueue(next);
        }
    }

    if (result.bounds.size() != displaySizes.size())
        return failure(QStringLiteral("The placement graph is disconnected or cyclic"));

    const QList<DisplayId> ids = result.bounds.keys();
    for (int i = 0; i < ids.size(); ++i) {
        const QRect bounds = result.bounds.value(ids.at(i));
        if (std::abs(int64_t(bounds.x())) > maximumCoordinate ||
            std::abs(int64_t(bounds.y())) > maximumCoordinate ||
            std::abs(int64_t(bounds.x()) + bounds.width()) > maximumCoordinate ||
            std::abs(int64_t(bounds.y()) + bounds.height()) > maximumCoordinate) {
            return failure(QStringLiteral("The resolved layout is outside the supported range"),
                           {ids.at(i)});
        }
        for (int j = i + 1; j < ids.size(); ++j) {
            if (bounds.intersects(result.bounds.value(ids.at(j))))
                return failure(QStringLiteral("Displays overlap after resolving the layout"),
                               {ids.at(i), ids.at(j)});
        }
    }

    result.succeeded = true;
    result.placements = std::move(normalized);
    return result;
}

} // namespace cutefish::display
