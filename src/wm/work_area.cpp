// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/work_area.h"

#include <algorithm>

namespace cutefish::wm {

bool ExclusiveZones::set(const void* token, ScreenEdge edge, int thickness,
                         const std::string& outputName)
{
    if (!token)
        return false;
    if (thickness <= 0)
        return clear(token);

    const Zone zone {edge, thickness, outputName};
    auto it = m_zones.find(token);
    if (it != m_zones.end() && it->second.edge == zone.edge
        && it->second.thickness == zone.thickness &&
        it->second.outputName == zone.outputName) {
        return false;
    }
    m_zones[token] = zone;
    return true;
}

bool ExclusiveZones::clear(const void* token)
{
    return m_zones.erase(token) != 0;
}

base::Rect ExclusiveZones::apply(const base::Rect& outputBounds) const
{
    return apply(outputBounds, std::string(), true);
}

base::Rect ExclusiveZones::apply(const base::Rect& outputBounds, const std::string& outputName,
                            bool primary) const
{
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;

    for (const auto& [token, zone] : m_zones) {
        if ((!zone.outputName.empty() && zone.outputName != outputName) ||
            (zone.outputName.empty() && !primary)) {
            continue;
        }
        switch (zone.edge) {
        case ScreenEdge::Top:
            top = std::max(top, zone.thickness);
            break;
        case ScreenEdge::Bottom:
            bottom = std::max(bottom, zone.thickness);
            break;
        case ScreenEdge::Left:
            left = std::max(left, zone.thickness);
            break;
        case ScreenEdge::Right:
            right = std::max(right, zone.thickness);
            break;
        }
    }

    base::Rect area = outputBounds.adjusted(left, top, -right, -bottom);
    // A reservation larger than the output would leave nothing to lay windows
    // out in; the output itself is the floor.
    if (area.width() <= 0 || area.height() <= 0)
        return outputBounds;
    return area;
}

} // namespace cutefish::wm
