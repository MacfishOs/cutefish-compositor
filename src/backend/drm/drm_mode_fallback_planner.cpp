// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/drm_gpu_display_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: a deterministic breadth-first search bounds driver
// tests and keeps internal panels fixed while MST branches shed bandwidth.

#include "backend/drm/drm_mode_fallback_planner.h"

#include <algorithm>
#include <deque>

#include <QSet>

namespace cutefish::backend {
namespace {

QList<DrmModeCandidate> sortedModes(const DrmFallbackHead &head)
{
    QList<DrmModeCandidate> modes = head.modes;
    std::stable_sort(modes.begin(), modes.end(), [](const auto &left, const auto &right) {
        if (left.preferred != right.preferred)
            return left.preferred;
        const qint64 leftArea = qint64(left.width) * left.height;
        const qint64 rightArea = qint64(right.width) * right.height;
        if (leftArea != rightArea)
            return leftArea > rightArea;
        if (left.refreshMilliHz != right.refreshMilliHz)
            return left.refreshMilliHz > right.refreshMilliHz;
        return left.key < right.key;
    });
    return modes;
}

QString stateKey(const QList<int> &indexes)
{
    QStringList values;
    for (int index : indexes)
        values.append(QString::number(index));
    return values.join(QLatin1Char(','));
}

} // namespace

std::optional<DrmFallbackPlan> DrmModeFallbackPlanner::choose(const QList<DrmFallbackHead> &heads,
                                                              Tester tester, int maximumAttempts)
{
    if (heads.isEmpty() || !tester || maximumAttempts <= 0)
        return std::nullopt;
    QList<QList<DrmModeCandidate>> modes;
    for (const auto &head : heads) {
        modes.append(sortedModes(head));
        if (modes.last().isEmpty())
            return std::nullopt;
    }

    std::deque<QList<int>> queue;
    queue.push_back(QList<int>(heads.size(), 0));
    QSet<QString> visited;
    int attempts = 0;
    while (!queue.empty() && attempts < maximumAttempts) {
        QList<int> indexes = queue.front();
        queue.pop_front();
        if (visited.contains(stateKey(indexes)))
            continue;
        visited.insert(stateKey(indexes));
        QList<DrmModeCandidate> selected;
        for (int i = 0; i < heads.size(); ++i)
            selected.append(modes.at(i).at(indexes.at(i)));
        const DrmFallbackTestResult tested = tester(selected);
        ++attempts;
        if (tested.succeeded) {
            DrmFallbackPlan result;
            result.modes = selected;
            result.attempts = attempts;
            result.diagnostic = tested.message;
            for (int i = 0; i < heads.size(); ++i)
                if (indexes.at(i) > 0)
                    result.downgradedConnectors.append(heads.at(i).connectorName);
            return result;
        }

        if (!tested.retryable)
            return std::nullopt;

        for (int i = 0; i < heads.size(); ++i) {
            const auto &head = heads.at(i);
            const bool affected = tested.failedBaseConnectorKeys.isEmpty()
                    || tested.failedBaseConnectorKeys.contains(head.baseConnectorKey);
            if (!head.internal && affected && indexes.at(i) + 1 < modes.at(i).size()) {
                QList<int> next = indexes;
                ++next[i];
                queue.push_back(std::move(next));
            }
        }
    }
    return std::nullopt;
}

} // namespace cutefish::backend
