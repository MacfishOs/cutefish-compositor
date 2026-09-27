// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/root_window_transformers.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: maps one logical mirror source through a destination's
// letterboxed native content rectangle without depending on Aura windows.

#pragma once

#include <optional>

#include <QPointF>
#include <QRect>

#include "display/display_types.h"

namespace cutefish::display {

class MirrorTransform
{
public:
    MirrorTransform() = default;
    MirrorTransform(QRect logicalBounds, QRect nativeContent, DisplayRotation rotation);

    bool isValid() const;
    QPointF logicalToNative(const QPointF& point) const;
    std::optional<QPointF> nativeToLogical(const QPointF& point) const;

private:
    QPointF rotate(const QPointF& normalized) const;
    QPointF unrotate(const QPointF& normalized) const;

    QRect m_logicalBounds;
    QRect m_nativeContent;
    DisplayRotation m_rotation = DisplayRotation::Normal;
};

} // namespace cutefish::display
