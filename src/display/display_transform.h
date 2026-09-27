// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/root_window_transformers.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: one value object owns the global-logical/native
// forward and inverse transforms without Aura root windows.

#pragma once

#include <optional>

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>

#include "compositor/output_scale.h"
#include "display/display_types.h"

namespace cutefish::display {

class DisplayTransform
{
public:
    DisplayTransform() = default;
    DisplayTransform(QPoint logicalOrigin, QSize nativeSize, compositor::OutputScale scale,
                     DisplayRotation rotation);

    bool isValid() const;
    QPoint logicalOrigin() const { return m_logicalOrigin; }
    QSize nativeSize() const { return m_nativeSize; }
    QSize rotatedNativeSize() const;
    QSize logicalSize() const;
    QRect logicalBounds() const;
    compositor::OutputScale scale() const { return m_scale; }
    DisplayRotation rotation() const { return m_rotation; }

    bool containsGlobal(const QPointF& point) const;
    QPointF globalToNative(const QPointF& point) const;
    QPointF nativeToGlobal(const QPointF& point) const;
    QRectF globalToNative(const QRectF& rect) const;
    QRectF nativeToGlobal(const QRectF& rect) const;

private:
    QPointF rotatedToNative(const QPointF& point) const;
    QPointF nativeToRotated(const QPointF& point) const;

    QPoint m_logicalOrigin;
    QSize m_nativeSize;
    compositor::OutputScale m_scale;
    DisplayRotation m_rotation = DisplayRotation::Normal;
};

} // namespace cutefish::display
