// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "display/display_topology_coordinator.h"
#include "display/recommended_scale.h"

#include <QSet>
#include <algorithm>

namespace cutefish::display {

DisplayTopologyCoordinator::DisplayTopologyCoordinator(DisplayManager *manager,
                                                       CallbackNativeDisplayDelegate *delegate,
                                                       DisplayConfigurationController *controller,
                                                       Probe probe, DisplayPreferences preferences,
                                                       QObject *parent)
    : QObject(parent)
    , m_manager(manager)
    , m_delegate(delegate)
    , m_controller(controller)
    , m_probe(std::move(probe))
    , m_preferences(std::move(preferences))
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &DisplayTopologyCoordinator::run);
    connect(
            controller, &DisplayConfigurationController::operationFinished, this,
            [this](const QString &token, const ConfigurationResult &result) {
                if (m_running && token == m_operationToken) {
                    m_running = false;
                    if (!m_pending)
                        m_controller->setTopologyPending(false);
                    emit reconciled(result.succeeded);
                }
                if (m_pending)
                    m_retry.start(0);
            },
            Qt::QueuedConnection);
}

void DisplayTopologyCoordinator::invalidate()
{
    if (!m_pending && !m_running && m_manager->applied()
        && !m_manager->applied()->logicalDisplays.isEmpty())
        m_previous = m_controller->recoveryState();
    schedule();
}

void DisplayTopologyCoordinator::start()
{
    m_previous.reset();
    schedule();
}

void DisplayTopologyCoordinator::schedule()
{
    m_pending = true;
    m_attempts = 0;
    m_controller->setTopologyPending(true);
    m_delegate->invalidateSnapshots();
    if (!m_retry.isActive())
        m_retry.start(100);
}

DisplayConfiguration DisplayTopologyCoordinator::candidate(const HardwareSnapshot &hardware,
                                                           uint64_t revision) const
{
    if (auto saved = m_preferences.restore(hardware, revision)) {
        if (DisplayConfigurationValidator::validateExtended(hardware, revision, *saved).succeeded)
            return *saved;
    }
    DisplayConfiguration result;
    result.base = {hardware.compositorInstanceId, hardware.topologyGeneration, revision};
    QSet<DisplayId> active;
    for (const auto &physical : hardware.displays) {
        const LogicalDisplay *logical = nullptr;
        if (m_previous) {
            for (const auto &old : m_previous->logicalDisplays) {
                if (old.destinations.contains(physical.sessionId)) {
                    logical = &old;
                    break;
                }
            }
        }
        auto mode = physical.nativeMode.value_or(
                physical.currentMode.value_or(physical.modes.first()));
        bool previouslyKnown = false;
        if (m_previous) {
            for (const auto &old : m_previous->physicalDisplays) {
                if (old.sessionId != physical.sessionId)
                    continue;
                previouslyKnown = true;
                if (old.currentMode) {
                    for (const auto &available : physical.modes)
                        if (available.stableKey() == old.currentMode->stableKey())
                            mode = available;
                }
            }
        }
        const bool enabled = logical || !previouslyKnown;
        result.displays.append({physical.sessionId, enabled, mode,
                                logical ? logical->scale : recommendedScale(physical),
                                logical ? logical->rotation : DisplayRotation::Normal});
        if (enabled)
            active.insert(physical.sessionId);
        if (logical && logical->primary && result.primary.value().isEmpty())
            result.primary = physical.sessionId;
    }
    if (active.isEmpty() && !result.displays.isEmpty()) {
        result.displays.first().enabled = true;
        active.insert(result.displays.first().display);
    }
    if (!active.contains(result.primary)) {
        for (const auto &request : result.displays) {
            if (request.enabled) {
                result.primary = request.display;
                break;
            }
        }
    }
    QSet<DisplayId> mirrorSinks;
    if (m_previous) {
        for (const auto &logical : m_previous->logicalDisplays) {
            QList<DisplayId> group;
            for (const auto &id : logical.destinations)
                if (active.contains(id))
                    group.append(id);
            if (group.size() > 1) {
                result.mirrorGroups.append(group);
                for (const auto &sink : group.mid(1))
                    mirrorSinks.insert(sink);
                if (group.contains(result.primary))
                    result.primary = group.first();
            }
        }
        for (const auto &placement : m_previous->placements) {
            if (active.contains(placement.display) && active.contains(placement.parent)
                && !mirrorSinks.contains(placement.display)
                && !mirrorSinks.contains(placement.parent))
                result.placements.append(placement);
        }
    }
    if (DisplayConfigurationValidator::validateExtended(hardware, revision, result).succeeded)
        return result;
    result.placements.clear();
    DisplayId parent = result.primary;
    for (const auto &request : result.displays) {
        if (!request.enabled || request.display == result.primary
            || mirrorSinks.contains(request.display))
            continue;
        result.placements.append({request.display, parent, LayoutEdge::Right, 0});
        parent = request.display;
    }
    return result;
}

void DisplayTopologyCoordinator::run()
{
    if (!m_pending || m_running || m_controller->busy())
        return;
    if (m_manager->applied())
        m_revision = std::max(m_revision, m_manager->applied()->revision);
    m_manager->clearApplied(m_manager->hardware()->topologyGeneration);
    auto hardware = m_probe();
    if (!hardware || !hardware->isValid()) {
        if (++m_attempts < 3)
            m_retry.start(250);
        else {
            m_pending = false;
            m_controller->setTopologyPending(false);
            emit reconciled(false);
        }
        return;
    }
    const uint64_t revision = ++m_revision;
    const auto request = candidate(*hardware, revision);
    if (!m_manager->installHardware(*hardware)) {
        emit reconciled(false);
        return;
    }
    m_delegate->setSnapshot(*hardware);
    AppliedConfiguration empty;
    empty.compositorInstanceId = hardware->compositorInstanceId;
    empty.topologyGeneration = hardware->topologyGeneration;
    empty.revision = revision;
    empty.physicalDisplays = hardware->displays;
    for (auto &physical : empty.physicalDisplays)
        physical.currentMode.reset();
    if (!m_manager->installApplied(empty)) {
        emit reconciled(false);
        return;
    }
    m_pending = false;
    if (hardware->displays.isEmpty()) {
        m_controller->setTopologyPending(false);
        emit reconciled(true);
        return;
    }
    m_running = true;
    m_operationToken = m_controller->apply(request, QStringLiteral("topology"), -1);
}

} // namespace cutefish::display
