// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/display_layout.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: IDs and geometry use Qt value types, and validation
// returns a normalized immutable result rather than mutating display objects.

#pragma once

#include <QHash>
#include <QList>
#include <QRect>
#include <QString>

#include "display/display_types.h"

namespace cutefish::display {

class DisplayLayout
{
public:
    static constexpr int maximumCoordinate = 100000;
    static constexpr int preferredMinimumSharedEdge = 32;
    static constexpr int maximumDisplayCount = 64;

    struct Result {
        bool succeeded = false;
        DisplayError error;
        QHash<DisplayId, QRect> bounds;
        QList<DisplayPlacement> placements;
    };

    static Result resolve(const DisplayId& primary, const QHash<DisplayId, QSize>& displaySizes,
                          const QList<DisplayPlacement>& placements,
                          int minimumSharedEdge = preferredMinimumSharedEdge);
};

} // namespace cutefish::display
