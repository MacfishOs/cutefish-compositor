// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/drm_gpu_display_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: device-local tests precede ordered commits, partial
// success rolls back, and unsupported cross-GPU/tile routes stay unavailable.

#include "backend/drm/drm_multi_device_planner.h"

#include <algorithm>

#include <QHash>
#include <QSet>

namespace cutefish::backend {

DrmMultiDeviceResult
DrmMultiDeviceCommitCoordinator::run(const QList<DrmDeviceTransaction> &devices,
                                     uint64_t expectedGeneration,
                                     std::function<uint64_t()> currentGeneration)
{
    if (devices.isEmpty() || !currentGeneration)
        return {false, false, {}, QStringLiteral("No DRM devices were supplied")};
    for (const auto &device : devices) {
        if (currentGeneration() != expectedGeneration)
            return {false, false, device.deviceKey,
                    QStringLiteral("DRM topology changed during testing")};
        if (!device.test || !device.test())
            return {false, false, device.deviceKey, QStringLiteral("A device test commit failed")};
    }
    QList<const DrmDeviceTransaction *> committed;
    const auto recover = [&](const QString &failedDevice) {
        bool restored = !committed.isEmpty();
        for (auto it = committed.crbegin(); it != committed.crend(); ++it) {
            const bool success = (*it)->rollback && (*it)->rollback();
            restored = restored && success;
        }
        return DrmMultiDeviceResult {
                false, restored, failedDevice,
                committed.isEmpty()
                        ? QStringLiteral("A device commit was rejected before any changes")
                        : restored
                        ? QStringLiteral("A device commit failed and prior devices were restored")
                        : QStringLiteral("A device commit failed and rollback was incomplete")};
    };
    for (const auto &device : devices) {
        if (currentGeneration() != expectedGeneration || !device.commit || !device.commit())
            return recover(device.deviceKey);
        committed.append(&device);
        if (currentGeneration() != expectedGeneration)
            return recover(device.deviceKey);
    }
    return {true, false, {}, {}};
}

CrossGpuTransfer chooseCrossGpuTransfer(const CrossGpuCapabilities &capabilities,
                                        qint64 maximumCpuCopyPixels)
{
    if (capabilities.sameDevice)
        return CrossGpuTransfer::Direct;
    if (capabilities.dmaBufImport)
        return CrossGpuTransfer::DmaBufImport;
    if (capabilities.gpuCopy)
        return CrossGpuTransfer::GpuCopy;
    if (capabilities.cpuMappable && capabilities.framePixels > 0
        && capabilities.framePixels <= maximumCpuCopyPixels)
        return CrossGpuTransfer::CpuCopy;
    return CrossGpuTransfer::Unavailable;
}

std::optional<QList<DrmTiledMonitor>> groupDrmTiles(const QList<DrmTileSnapshot> &tiles)
{
    QHash<QString, QList<DrmTileSnapshot>> groups;
    for (const auto &tile : tiles) {
        if (!tile.groupId.isEmpty())
            groups[tile.groupId].append(tile);
    }
    QList<DrmTiledMonitor> result;
    QStringList keys = groups.keys();
    std::sort(keys.begin(), keys.end());
    for (const auto &key : keys) {
        auto group = groups.value(key);
        const auto first = group.first();
        if (first.columns < 1 || first.rows < 1 || first.tileSize.isEmpty()
            || group.size() != first.columns * first.rows)
            return std::nullopt;
        QSet<int> positions;
        for (const auto &tile : group) {
            if (tile.columns != first.columns || tile.rows != first.rows
                || tile.tileSize != first.tileSize || tile.column < 0 || tile.column >= tile.columns
                || tile.row < 0 || tile.row >= tile.rows)
                return std::nullopt;
            positions.insert(tile.row * tile.columns + tile.column);
        }
        if (positions.size() != group.size())
            return std::nullopt;
        std::sort(group.begin(), group.end(), [](const auto &left, const auto &right) {
            return left.row != right.row ? left.row < right.row : left.column < right.column;
        });
        DrmTiledMonitor monitor {
                key,
                QSize(first.tileSize.width() * first.columns, first.tileSize.height() * first.rows),
                {}};
        for (const auto &tile : group)
            monitor.connectors.append(tile.connectorId);
        result.append(monitor);
    }
    return result;
}

bool hardwareCloneEligible(const QString &firstDevice, const QString &secondDevice,
                           const QString &firstMode, const QString &secondMode,
                           uint32_t firstPossibleCrtcs, uint32_t secondPossibleCrtcs)
{
    return !firstDevice.isEmpty() && firstDevice == secondDevice && !firstMode.isEmpty()
            && firstMode == secondMode && (firstPossibleCrtcs & secondPossibleCrtcs) != 0;
}

} // namespace cutefish::backend
