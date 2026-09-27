// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/types/display_snapshot.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: validate stable Qt snapshots and exact timing-based
// mode identities without Chromium display or gfx dependencies.

#include "display/display_types.h"

#include <QSet>

namespace cutefish::display {

size_t qHash(const DisplayId& id, size_t seed) noexcept
{
    return ::qHash(id.value(), seed);
}

bool DisplayMode::isValid() const
{
    return pixelSize.width() > 0 && pixelSize.height() > 0 && refreshMilliHz > 0 &&
        horizontalTotal >= pixelSize.width() && verticalTotal >= pixelSize.height();
}

QString DisplayMode::stableKey() const
{
    return QStringLiteral("%1x%2@%3:%4:%5:%6:%7")
        .arg(pixelSize.width())
        .arg(pixelSize.height())
        .arg(refreshMilliHz)
        .arg(pixelClockKHz)
        .arg(horizontalTotal)
        .arg(verticalTotal)
        .arg(flags);
}

bool DisplaySnapshot::isValid() const
{
    if (!sessionId.isValid() || persistentKey.isEmpty() || portKey.isEmpty() ||
        connectorName.isEmpty() || devicePath.isEmpty() || physicalSizeMm.width() < 0 ||
        physicalSizeMm.height() < 0 || modes.isEmpty()) {
        return false;
    }

    QSet<QString> modeKeys;
    for (const DisplayMode& mode : modes) {
        if (!mode.isValid() || modeKeys.contains(mode.stableKey()))
            return false;
        modeKeys.insert(mode.stableKey());
    }
    return (!currentMode || modeKeys.contains(currentMode->stableKey())) &&
        (!nativeMode || modeKeys.contains(nativeMode->stableKey()));
}

bool HardwareSnapshot::isValid() const
{
    if (compositorInstanceId.isEmpty() || seatId.isEmpty() || topologyGeneration == 0)
        return false;

    QSet<DisplayId> sessionIds;
    QSet<QString> portKeys;
    for (const DisplaySnapshot& display : displays) {
        if (!display.isValid() || sessionIds.contains(display.sessionId) ||
            portKeys.contains(display.portKey)) {
            return false;
        }
        sessionIds.insert(display.sessionId);
        portKeys.insert(display.portKey);
    }
    return true;
}

} // namespace cutefish::display
