// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "backend/drm/drm_selection.h"

#include <algorithm>
#include <utility>

#include <xf86drm.h>

namespace cutefish::backend {

bool isDesktopOutput(const ConnectorInfo& connector)
{
    return connector.connection == DRM_MODE_CONNECTED && connector.modeCount > 0
        && !connector.nonDesktop;
}

std::optional<int> selectConnector(const QList<ConnectorInfo>& connectors)
{
    for (int i = 0; i < connectors.size(); ++i) {
        if (isDesktopOutput(connectors.at(i)))
            return i;
    }
    return std::nullopt;
}

QStringList orderDisplayDevices(const QList<DrmDeviceCandidate>& candidates)
{
    static const QStringList preferred = {QStringLiteral("i915"), QStringLiteral("amdgpu"),
                                          QStringLiteral("virtio_gpu")};
    const auto rank = [](const DrmDeviceCandidate& candidate) {
        const auto index = preferred.indexOf(candidate.driver);
        return std::pair(candidate.bootVga ? 0 : 1, index < 0 ? preferred.size() : index);
    };
    QList<DrmDeviceCandidate> ordered = candidates;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const DrmDeviceCandidate& a, const DrmDeviceCandidate& b) {
                         return rank(a) < rank(b);
                     });
    QStringList paths;
    for (const DrmDeviceCandidate& candidate : ordered)
        paths.append(candidate.path);
    return paths;
}

std::optional<drmModeModeInfo> selectMode(const drmModeModeInfo* modes, int count)
{
    if (!modes || count <= 0)
        return std::nullopt;

    const auto interlaced = [](const drmModeModeInfo& mode) {
        return (mode.flags & DRM_MODE_FLAG_INTERLACE) != 0;
    };
    const drmModeModeInfo* best = nullptr;
    for (int i = 0; i < count; ++i) {
        const drmModeModeInfo& mode = modes[i];
        if (mode.type & DRM_MODE_TYPE_PREFERRED)
            return mode;

        if (!best) {
            best = &mode;
            continue;
        }
        // Interlaced scanout flickers on text and is only a last resort.
        if (interlaced(mode) != interlaced(*best)) {
            if (!interlaced(mode))
                best = &mode;
            continue;
        }

        const long bestArea = static_cast<long>(best->hdisplay) * best->vdisplay;
        const long area = static_cast<long>(mode.hdisplay) * mode.vdisplay;
        if (area > bestArea || (area == bestArea && refreshRate(mode) > refreshRate(*best)))
            best = &mode;
    }

    return *best;
}

uint32_t selectCrtc(uint32_t possibleCrtcs,
                    const QList<uint32_t>& crtcIds,
                    uint32_t usedCrtcMask,
                    uint32_t currentCrtcId)
{
    uint32_t first = 0;
    for (int i = 0; i < crtcIds.size(); ++i) {
        const uint32_t bit = 1u << i;
        if (!(possibleCrtcs & bit) || (usedCrtcMask & bit))
            continue;
        if (crtcIds.at(i) == currentCrtcId && currentCrtcId != 0)
            return currentCrtcId;
        if (!first)
            first = crtcIds.at(i);
    }
    return first;
}

double refreshRate(const drmModeModeInfo& mode)
{
    if (mode.htotal == 0 || mode.vtotal == 0)
        return 0.0;

    double total = static_cast<double>(mode.htotal) * mode.vtotal;
    if (mode.flags & DRM_MODE_FLAG_INTERLACE)
        total /= 2.0;
    if (mode.flags & DRM_MODE_FLAG_DBLSCAN)
        total *= 2.0;
    if (mode.vscan > 1)
        total *= mode.vscan;

    return mode.clock * 1000.0 / total;
}

QString connectorName(uint32_t type, uint32_t typeId)
{
    const char* name = drmModeGetConnectorTypeName(type);
    return QStringLiteral("%1-%2")
        .arg(QString::fromUtf8(name ? name : "Unknown"))
        .arg(typeId);
}

} // namespace cutefish::backend
