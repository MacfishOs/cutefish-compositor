// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/drm_gpu_display_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: bounded combined-mode retries downgrade only affected
// external base-connector groups and return structured link diagnostics.

#pragma once

#include <functional>
#include <optional>

#include <QList>
#include <QString>

namespace cutefish::backend {

struct DrmModeCandidate {
    QString key;
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    int pixelClockKHz = 0;
    bool preferred = false;
};

struct DrmFallbackHead {
    uint32_t connectorId = 0;
    QString connectorName;
    QString baseConnectorKey;
    bool internal = false;
    QList<DrmModeCandidate> modes;
};

struct DrmFallbackTestResult {
    bool succeeded = false;
    QStringList failedBaseConnectorKeys;
    QString message;
    bool retryable = true;
};

struct DrmFallbackPlan {
    QList<DrmModeCandidate> modes;
    QStringList downgradedConnectors;
    QString diagnostic;
    int attempts = 0;
};

class DrmModeFallbackPlanner
{
public:
    using Tester = std::function<DrmFallbackTestResult(const QList<DrmModeCandidate> &)>;
    static std::optional<DrmFallbackPlan> choose(const QList<DrmFallbackHead> &heads, Tester tester,
                                                 int maximumAttempts = 64);
};

} // namespace cutefish::backend
