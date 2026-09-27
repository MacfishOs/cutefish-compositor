// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/extended_mouse_warp_controller.cc
//           chromium/src/ash/display/root_window_transformers.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: route cached cursor pixels to every matching physical
// mirror sink and translate the global position for each destination.

#include "cursor/output_cursor_router.h"

#include <QtMath>

namespace cutefish::cursor {

namespace {

bool contains(const QRect &bounds, const QPointF &position)
{
    return bounds.contains(QPoint(qFloor(position.x()), qFloor(position.y())));
}

} // namespace

void OutputCursorRouter::setTargets(std::vector<Target> targets)
{
    const bool visible = !m_activeTargets.empty();
    hideCursor();
    m_targets = std::move(targets);
    m_activeTargets.clear();
    if (visible)
        moveCursor(m_position);
}

bool OutputCursorRouter::upload(int index)
{
    if (index < 0 || index >= static_cast<int>(m_targets.size()) || !m_targets[index].sink ||
        m_pixels.isEmpty()) {
        return false;
    }
    m_image.pixels = reinterpret_cast<const uint8_t*>(m_pixels.constData());
    return m_targets[index].sink->setCursorImage(m_image);
}

bool OutputCursorRouter::setCursorImage(const render::CursorImage& image)
{
    if (!image.pixels || image.size.isEmpty() || image.stride <= 0)
        return false;
    m_pixels =
        QByteArray(reinterpret_cast<const char*>(image.pixels), image.stride * image.size.height());
    m_image = image;
    m_image.pixels = reinterpret_cast<const uint8_t*>(m_pixels.constData());
    std::vector<int> targets;
    for (int index = 0; index < static_cast<int>(m_targets.size()); ++index)
        if (contains(m_targets[index].logicalBounds, m_position))
            targets.push_back(index);
    if (targets.empty() && !m_targets.empty())
        targets.push_back(0);
    bool uploaded = false;
    for (int target : targets)
        if (upload(target)) {
            m_activeTargets.push_back(target);
            uploaded = true;
        }
    return uploaded;
}

void OutputCursorRouter::hideCursor()
{
    for (int target : m_activeTargets)
        if (target >= 0 && target < static_cast<int>(m_targets.size()) && m_targets[target].sink)
            m_targets[target].sink->hideCursor();
    m_activeTargets.clear();
}

void OutputCursorRouter::moveCursor(const QPointF& position)
{
    m_position = position;
    std::vector<int> targets;
    for (int index = 0; index < static_cast<int>(m_targets.size()); ++index) {
        if (contains(m_targets[index].logicalBounds, position))
            targets.push_back(index);
    }
    if (targets.empty())
        return;
    if (targets != m_activeTargets) {
        hideCursor();
        for (int target : targets)
            if (upload(target))
                m_activeTargets.push_back(target);
    }
    for (int target : m_activeTargets)
        m_targets[target].sink->moveCursor(
            m_targets[target].mirrorTransform.isValid()
                ? m_targets[target].mirrorTransform.logicalToNative(position)
                : position - m_targets[target].logicalBounds.topLeft());
}

} // namespace cutefish::cursor
