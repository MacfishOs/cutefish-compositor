// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/drm_gpu_display_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: cross-device commits, framebuffer transfer fallback,
// tiled monitor grouping, and clone eligibility are explicit testable plans.

#pragma once

#include <functional>
#include <optional>

#include <QList>
#include <QSize>
#include <QString>

namespace cutefish::backend {

struct DrmDeviceTransaction {
    QString deviceKey;
    std::function<bool()> test;
    std::function<bool()> commit;
    std::function<bool()> rollback;
};

struct DrmMultiDeviceResult {
    bool succeeded = false;
    bool rolledBack = false;
    QString failedDevice;
    QString message;
};

class DrmMultiDeviceCommitCoordinator
{
public:
    static DrmMultiDeviceResult run(const QList<DrmDeviceTransaction>& devices,
                                    uint64_t expectedGeneration,
                                    std::function<uint64_t()> currentGeneration);
};

// The largest frame copied through the CPU when the scanout device cannot
// import it: a 4K screen still gets a picture, at the cost of a copy a frame.
inline constexpr qint64 kMaximumCpuCopyPixels = 3840 * 2160;

enum class CrossGpuTransfer {
    Direct,
    DmaBufImport,
    GpuCopy,
    CpuCopy,
    Unavailable
};

struct CrossGpuCapabilities {
    bool sameDevice = false;
    bool dmaBufImport = false;
    bool gpuCopy = false;
    bool cpuMappable = false;
    qint64 framePixels = 0;
};

CrossGpuTransfer chooseCrossGpuTransfer(const CrossGpuCapabilities& capabilities,
                                        qint64 maximumCpuCopyPixels = kMaximumCpuCopyPixels);

struct DrmTileSnapshot {
    uint32_t connectorId = 0;
    QString groupId;
    int column = 0;
    int row = 0;
    int columns = 1;
    int rows = 1;
    QSize tileSize;
};

struct DrmTiledMonitor {
    QString groupId;
    QSize combinedSize;
    QList<uint32_t> connectors;
};

std::optional<QList<DrmTiledMonitor>> groupDrmTiles(const QList<DrmTileSnapshot>& tiles);

bool hardwareCloneEligible(const QString& firstDevice, const QString& secondDevice,
                           const QString& firstMode, const QString& secondMode,
                           uint32_t firstPossibleCrtcs, uint32_t secondPossibleCrtcs);

} // namespace cutefish::backend
