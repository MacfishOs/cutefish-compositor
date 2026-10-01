// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/cros_display_config.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: typed D-Bus calls are authorized by peer credentials
// and active-seat state before entering the shared transaction authority.

#include "ipc/display_config_service.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>

#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QUuid>

#include "display/recommended_scale.h"
#include "display_config_adaptor.h"

namespace cutefish::display::ipc {

DisplayConfigService::DisplayConfigService(DisplayManager* manager,
                                           DisplayConfigurationController* controller,
                                           QDBusConnection connection, SessionActive sessionActive,
                                           QObject* parent)
    : QObject(parent)
    , m_manager(manager)
    , m_controller(controller)
    , m_connection(std::move(connection))
    , m_sessionActive(std::move(sessionActive))
{
    registerTypes();
    new DisplayConfigAdaptor(this);
    connect(m_manager, &DisplayManager::displaysChanged, this, &DisplayConfigService::StateChanged);
    connect(m_controller, &DisplayConfigurationController::previewChanged, this,
            [this](const QString& token, qint64 deadline) {
                const auto generation = m_manager->hardware() ? m_manager->hardware()->topologyGeneration : 0;
                const auto revision = m_manager->applied() ? m_manager->applied()->revision : 0;
                emit PreviewChanged(token, deadline, generation, revision);
            });
    connect(m_controller, &DisplayConfigurationController::operationFinished, this,
            [this](const QString& token, const ConfigurationResult& result) {
                QStringList affected;
                for (const auto& id : result.error.affectedDisplays)
                    affected.append(id.value());
                const auto generation = m_manager->hardware() ? m_manager->hardware()->topologyGeneration : 0;
                const auto revision = m_manager->applied() ? m_manager->applied()->revision : 0;
                emit OperationFinished(token, result.succeeded, result.degraded,
                                       uint(result.error.code), result.error.message, affected,
                                       generation, revision);
            });
    if (m_connection.interface()) {
        connect(m_connection.interface(), &QDBusConnectionInterface::serviceOwnerChanged, this,
                [this](const QString& name, const QString& oldOwner, const QString& newOwner) {
                    if (!oldOwner.isEmpty() && newOwner.isEmpty())
                        m_controller->ownerDisconnected(name);
                });
    }
}

bool DisplayConfigService::registerService(const QString& serviceName)
{
    if (!m_connection.isConnected() || serviceName.isEmpty())
        return false;
    m_serviceName = serviceName;
    return m_connection.registerService(serviceName) &&
        m_connection.registerObject(QStringLiteral("/com/cutefish/DisplayConfig1"), this,
                                    QDBusConnection::ExportAdaptors);
}

QString DisplayConfigService::caller() const
{
    return calledFromDBus() ? message().service() : QStringLiteral("internal");
}

bool DisplayConfigService::authorizeWrite()
{
    if (!m_sessionActive()) {
        sendErrorReply(QStringLiteral("com.cutefish.DisplayConfig1.Error.Unauthorized"),
                       QStringLiteral("The graphical session is inactive"));
        return false;
    }
    return authorizeCaller();
}

bool DisplayConfigService::authorizeCaller()
{
    if (!calledFromDBus())
        return true;
    const QDBusReply<uint> uid = m_connection.interface()->serviceUid(message().service());
    if (!uid.isValid() || uid.value() != uint(getuid())) {
        sendErrorReply(QStringLiteral("com.cutefish.DisplayConfig1.Error.Unauthorized"),
                       QStringLiteral("The caller does not own this user session"));
        return false;
    }
    return true;
}

QString DisplayConfigService::GetState(QString& seatId, qulonglong& topologyGeneration,
                                       qulonglong& appliedRevision,
                                       QList<PhysicalDisplay>& physicalDisplays,
                                       QList<LogicalDisplay>& logicalDisplays,
                                       QList<Placement>& placements, QString& previewToken,
                                       qulonglong& previewDeadlineMs)
{
    const auto hardware = m_manager->hardware();
    const auto applied = m_manager->applied();
    seatId = hardware ? hardware->seatId : QString();
    topologyGeneration = hardware ? hardware->topologyGeneration : 0;
    appliedRevision = applied ? applied->revision : 0;
    physicalDisplays = toWire(applied ? applied->physicalDisplays :
        (hardware ? hardware->displays : QList<display::DisplaySnapshot>()));
    logicalDisplays = toWire(applied ? applied->logicalDisplays : QList<display::LogicalDisplay>());
    placements = toWire(applied ? applied->placements : QList<display::DisplayPlacement>());
    previewToken = m_controller->previewToken();
    previewDeadlineMs = m_controller->previewDeadlineMs();
    return hardware ? hardware->compositorInstanceId : QString();
}

QString DisplayConfigService::reject(DisplayErrorCode code, const QString& message)
{
    const QString token = QStringLiteral("rejected-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto generation = m_manager->hardware() ? m_manager->hardware()->topologyGeneration : 0;
    const auto revision = m_manager->applied() ? m_manager->applied()->revision : 0;
    emit OperationFinished(token, false, false, uint(code), message, {}, generation, revision);
    return token;
}

QString DisplayConfigService::TestConfiguration(
    const QString& instanceId, qulonglong topologyGeneration, qulonglong appliedRevision,
    const QList<DisplayRequest>& displays, const QString& primary,
    const QList<Placement>& placements, const MirrorGroups& mirrorGroups)
{
    if (!authorizeWrite())
        return {};
    return m_controller->test(fromWire(instanceId, topologyGeneration, appliedRevision, displays,
                                       primary, placements, mirrorGroups));
}

QString DisplayConfigService::ApplyConfiguration(
    const QString& instanceId, qulonglong topologyGeneration, qulonglong appliedRevision,
    const QList<DisplayRequest>& displays, const QString& primary,
    const QList<Placement>& placements, const MirrorGroups& mirrorGroups)
{
    if (!authorizeWrite())
        return {};
    return m_controller->apply(fromWire(instanceId, topologyGeneration, appliedRevision, displays,
                                        primary, placements, mirrorGroups), caller());
}

QString DisplayConfigService::SetConfiguration(
    const QString& instanceId, qulonglong topologyGeneration, qulonglong appliedRevision,
    const QList<DisplayRequest>& displays, const QString& primary,
    const QList<Placement>& placements, const MirrorGroups& mirrorGroups)
{
    if (!authorizeWrite())
        return {};
    return m_controller->apply(fromWire(instanceId, topologyGeneration, appliedRevision, displays,
                                        primary, placements, mirrorGroups), caller(), 0);
}

void DisplayConfigService::ConfirmConfiguration(const QString& previewToken)
{
    if (authorizeWrite())
        m_controller->confirm(previewToken, caller());
}

void DisplayConfigService::RevertConfiguration(const QString& previewToken)
{
    if (authorizeWrite())
        m_controller->revert(previewToken, caller());
}

DisplayConfiguration DisplayConfigService::defaultConfiguration(
    const QString& instanceId, qulonglong topologyGeneration, qulonglong appliedRevision) const
{
    DisplayConfiguration result;
    result.base = {instanceId, topologyGeneration, appliedRevision};
    if (!m_manager->hardware())
        return result;
    DisplayId parent;
    for (const auto& display : m_manager->hardware()->displays) {
        const auto mode = display.nativeMode ? display.nativeMode : display.currentMode;
        if (!mode)
            continue;
        result.displays.append({display.sessionId, true, *mode, recommendedScale(display),
                                DisplayRotation::Normal});
        if (!result.primary.isValid()) {
            result.primary = display.sessionId;
            parent = display.sessionId;
        } else {
            result.placements.append({display.sessionId, parent, LayoutEdge::Right, 0});
            parent = display.sessionId;
        }
    }
    return result;
}

QString DisplayConfigService::ResetConfiguration(const QString& instanceId,
                                                 qulonglong topologyGeneration,
                                                 qulonglong appliedRevision)
{
    if (!authorizeWrite())
        return {};
    return m_controller->apply(defaultConfiguration(instanceId, topologyGeneration, appliedRevision),
                               caller());
}

QString DisplayConfigService::SetDisplayPower(const QStringList& displayIds, uint state)
{
    if (!authorizeWrite())
        return {};
    if (state > uint(NativeDisplayDelegate::PowerState::Suspend))
        return reject(DisplayErrorCode::UnsupportedProperty,
                      QStringLiteral("The display power state is invalid"));
    QList<DisplayId> ids;
    for (const auto& value : displayIds)
        ids.append(DisplayId(value));
    return m_controller->setPower(ids, NativeDisplayDelegate::PowerState(state));
}

QString DisplayConfigService::SetDockedLidClosed(bool closed)
{
    if (!authorizeWrite())
        return {};
    if (!m_manager->hardware())
        return reject(DisplayErrorCode::Unavailable, QStringLiteral("Display hardware is unavailable"));
    // Between a hotplug and its new layout nothing is applied, which is not "no external display".
    if (closed && !m_manager->applied())
        return reject(DisplayErrorCode::Busy, QStringLiteral("A display change is in progress"));
    bool usableExternal = false;
    QList<DisplayId> internal;
    for (const auto& physical : m_manager->hardware()->displays) {
        if (physical.connectionType == DisplayConnectionType::Internal)
            internal.append(physical.sessionId);
        else if (closed && m_manager->applied()) {
            for (const auto& logical : m_manager->applied()->logicalDisplays)
                usableExternal |= logical.destinations.contains(physical.sessionId);
        }
    }
    if (closed && !usableExternal)
        return reject(DisplayErrorCode::NoEnabledOutput,
                      QStringLiteral("No active external display is available for docked mode"));
    return m_controller->setPower(internal, closed ? NativeDisplayDelegate::PowerState::Off
                                                   : NativeDisplayDelegate::PowerState::On);
}

void DisplayConfigService::IdentifyDisplays(const QStringList& displayIds)
{
    if (authorizeWrite())
        emit IdentifyRequested(displayIds);
}

void DisplayConfigService::SetNightLight(double strength, uint durationMs)
{
    // A tint, not a configuration: a session that is not on screen keeps it
    // for when it is back.
    if (!authorizeCaller())
        return;
    if (!std::isfinite(strength)) {
        if (calledFromDBus())
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("The strength is not a number"));
        return;
    }
    if (m_nightLight)
        m_nightLight(std::clamp(strength, 0.0, 1.0), std::chrono::milliseconds(durationMs));
}

} // namespace cutefish::display::ipc
