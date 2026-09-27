// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/screen_manager.cc
//           chromium/src/ui/ozone/platform/drm/gpu/drm_gpu_display_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: bounded backtracking preserves compatible routes and
// prevents independent outputs from claiming the same CRTC or primary plane.

#include "backend/drm/drm_configuration_planner.h"

#include <algorithm>

#include <QSet>

namespace cutefish::backend {
namespace {

bool assignRoute(int routeIndex, const QList<DrmRouteCandidate> &routes,
                 const QList<DrmCrtcCandidate> &crtcs, QSet<uint32_t> *usedCrtcs,
                 QSet<uint32_t> *usedPlanes, QList<DrmRouteAssignment> *result, int *skip)
{
    if (routeIndex == routes.size())
        return (*skip)-- == 0;

    const DrmRouteCandidate &route = routes.at(routeIndex);
    QList<const DrmCrtcCandidate *> choices;
    for (const DrmCrtcCandidate &crtc : crtcs) {
        if (crtc.index < 0 || crtc.index >= 32
            || !(route.possibleCrtcs & (uint32_t {1} << crtc.index))
            || usedCrtcs->contains(crtc.crtcId)) {
            continue;
        }
        choices.append(&crtc);
    }
    std::stable_sort(choices.begin(), choices.end(), [&](const auto *left, const auto *right) {
        return left->crtcId == route.currentCrtcId && right->crtcId != route.currentCrtcId;
    });

    for (const DrmCrtcCandidate *crtc : choices) {
        for (uint32_t plane : crtc->primaryPlanes) {
            if (!plane || usedPlanes->contains(plane))
                continue;
            usedCrtcs->insert(crtc->crtcId);
            usedPlanes->insert(plane);
            result->append({route.connectorId, crtc->crtcId, crtc->index, plane});
            if (assignRoute(routeIndex + 1, routes, crtcs, usedCrtcs, usedPlanes, result, skip))
                return true;
            result->removeLast();
            usedPlanes->remove(plane);
            usedCrtcs->remove(crtc->crtcId);
        }
    }
    return false;
}

} // namespace

std::optional<QList<DrmRouteAssignment>>
DrmConfigurationPlanner::assign(const QList<DrmRouteCandidate> &routes,
                                const QList<DrmCrtcCandidate> &crtcs, int alternative)
{
    if (alternative < 0 || alternative >= 8)
        return std::nullopt;
    if (routes.isEmpty())
        return alternative == 0 ? std::optional(QList<DrmRouteAssignment> {}) : std::nullopt;
    QList<DrmRouteAssignment> result;
    QSet<uint32_t> usedCrtcs;
    QSet<uint32_t> usedPlanes;
    if (!assignRoute(0, routes, crtcs, &usedCrtcs, &usedPlanes, &result, &alternative))
        return std::nullopt;
    return result;
}

} // namespace cutefish::backend
