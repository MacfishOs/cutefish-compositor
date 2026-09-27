// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/root_window_transformers.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: Qt point and rectangle mappings cover fractional
// scale, negative origins, and all compositor-supported rotations.

#include "display/display_transform.h"

#include <algorithm>
#include <array>

namespace cutefish::display {

namespace {

template<typename Mapper>
QRectF mapRectangle(const QRectF& rect, Mapper mapper)
{
    const std::array<QPointF, 4> points {
        mapper(rect.topLeft()), mapper(rect.topRight()), mapper(rect.bottomLeft()),
        mapper(rect.bottomRight())};
    double left = points[0].x();
    double right = left;
    double top = points[0].y();
    double bottom = top;
    for (const QPointF& point : points) {
        left = std::min(left, point.x());
        right = std::max(right, point.x());
        top = std::min(top, point.y());
        bottom = std::max(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

} // namespace

DisplayTransform::DisplayTransform(QPoint logicalOrigin, QSize nativeSize,
                                   compositor::OutputScale scale, DisplayRotation rotation)
    : m_logicalOrigin(logicalOrigin)
    , m_nativeSize(nativeSize)
    , m_scale(scale)
    , m_rotation(rotation)
{
}

bool DisplayTransform::isValid() const
{
    return m_nativeSize.width() > 0 && m_nativeSize.height() > 0;
}

QSize DisplayTransform::rotatedNativeSize() const
{
    if (m_rotation == DisplayRotation::Rotate90 || m_rotation == DisplayRotation::Rotate270)
        return m_nativeSize.transposed();
    return m_nativeSize;
}

QSize DisplayTransform::logicalSize() const
{
    return m_scale.logicalSize(rotatedNativeSize());
}

QRect DisplayTransform::logicalBounds() const
{
    return QRect(m_logicalOrigin, logicalSize());
}

bool DisplayTransform::containsGlobal(const QPointF& point) const
{
    const QSize extent = logicalSize();
    return point.x() >= m_logicalOrigin.x() && point.y() >= m_logicalOrigin.y() &&
        point.x() < m_logicalOrigin.x() + extent.width() &&
        point.y() < m_logicalOrigin.y() + extent.height();
}

QPointF DisplayTransform::globalToNative(const QPointF& point) const
{
    return rotatedToNative(m_scale.toDevice(point - QPointF(m_logicalOrigin)));
}

QPointF DisplayTransform::nativeToGlobal(const QPointF& point) const
{
    return m_scale.toLogical(nativeToRotated(point)) + QPointF(m_logicalOrigin);
}

QRectF DisplayTransform::globalToNative(const QRectF& rect) const
{
    return mapRectangle(rect, [this](const QPointF& point) { return globalToNative(point); });
}

QRectF DisplayTransform::nativeToGlobal(const QRectF& rect) const
{
    return mapRectangle(rect, [this](const QPointF& point) { return nativeToGlobal(point); });
}

QPointF DisplayTransform::rotatedToNative(const QPointF& point) const
{
    switch (m_rotation) {
    case DisplayRotation::Normal:
        return point;
    case DisplayRotation::Rotate90:
        return QPointF(point.y(), m_nativeSize.height() - point.x());
    case DisplayRotation::Rotate180:
        return QPointF(m_nativeSize.width() - point.x(), m_nativeSize.height() - point.y());
    case DisplayRotation::Rotate270:
        return QPointF(m_nativeSize.width() - point.y(), point.x());
    }
    return point;
}

QPointF DisplayTransform::nativeToRotated(const QPointF& point) const
{
    switch (m_rotation) {
    case DisplayRotation::Normal:
        return point;
    case DisplayRotation::Rotate90:
        return QPointF(m_nativeSize.height() - point.y(), point.x());
    case DisplayRotation::Rotate180:
        return QPointF(m_nativeSize.width() - point.x(), m_nativeSize.height() - point.y());
    case DisplayRotation::Rotate270:
        return QPointF(point.y(), m_nativeSize.width() - point.x());
    }
    return point;
}

} // namespace cutefish::display
