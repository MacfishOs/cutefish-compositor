// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/resolution_notification_controller.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: a server-side timer and client ownership guard protect
// complete layout previews, with rollback using the same native configurator.

#include "display/display_configuration_controller.h"

#include <QDateTime>
#include <QUuid>

namespace cutefish::display {
namespace {

const DisplaySnapshot *snapshotForId(const HardwareSnapshot &hardware, const DisplayId &id)
{
    for (const auto &display : hardware.displays) {
        if (display.sessionId == id)
            return &display;
    }
    return nullptr;
}

} // namespace

DisplayConfigurationController::DisplayConfigurationController(DisplayManager *manager,
                                                               NativeDisplayDelegate *delegate,
                                                               DisplayPreferences preferences,
                                                               QObject *parent)
    : QObject(parent)
    , m_manager(manager)
    , m_delegate(delegate)
    , m_configurator(delegate, this)
    , m_preferences(std::move(preferences))
{
    qRegisterMetaType<ConfigurationResult>();
    m_previewTimer.setSingleShot(true);
    connect(&m_previewTimer, &QTimer::timeout, this, [this] {
        startRevert(DisplayErrorCode::Cancelled, QStringLiteral("Display preview timed out"));
    });
    connect(m_delegate, &NativeDisplayDelegate::hardwareInvalidated, this,
            &DisplayConfigurationController::hardwareInvalidated);
}

QString DisplayConfigurationController::nextToken(const char *prefix) const
{
    return QString::fromLatin1(prefix) + QLatin1Char('-')
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void DisplayConfigurationController::finish(const QString &operationToken,
                                            ConfigurationResult result)
{
    m_operationActive = false;
    emit operationFinished(operationToken, std::move(result));
}

QString DisplayConfigurationController::test(const DisplayConfiguration &request)
{
    const QString token = nextToken("test");
    if (m_operationActive || m_preview || m_manager->isPublishing() || m_topologyPending) {
        emit operationFinished(
                token,
                {false,
                 false,
                 {DisplayErrorCode::Busy, QStringLiteral("A display preview is active"), {}},
                 std::nullopt});
        return token;
    }
    if (!m_manager->hardware()) {
        finish(token,
               {false,
                false,
                {DisplayErrorCode::Unavailable, QStringLiteral("No display hardware state"), {}},
                std::nullopt});
        return token;
    }
    m_operationActive = true;
    const uint64_t revision = m_manager->applied() ? m_manager->applied()->revision : 0;
    auto exact = request;
    exact.allowModeFallback = false;
    m_configurator.test(*m_manager->hardware(), revision, exact,
                        [this, token](ConfigurationResult result) mutable {
                            finish(token, std::move(result));
                        });
    return token;
}

QString DisplayConfigurationController::apply(const DisplayConfiguration &request,
                                              const QString &owner, int timeoutMs)
{
    const QString operationToken = nextToken("apply");
    if (m_operationActive || m_preview || m_manager->isPublishing()
        || (m_topologyPending && owner != QStringLiteral("topology"))) {
        emit operationFinished(
                operationToken,
                {false,
                 false,
                 {DisplayErrorCode::Busy, QStringLiteral("A display transaction is active"), {}},
                 std::nullopt});
        return operationToken;
    }
    if (!m_manager->hardware() || !m_manager->applied()) {
        finish(operationToken,
               {false,
                false,
                {DisplayErrorCode::Unavailable, QStringLiteral("No applied display state"), {}},
                std::nullopt});
        return operationToken;
    }
    const auto hardware = *m_manager->hardware();
    const auto previous = *m_manager->applied();
    auto exactRequest = request;
    exactRequest.allowModeFallback = owner == QStringLiteral("topology") && timeoutMs < 0;
    const auto validated = DisplayConfigurationValidator::validateExtended(
            hardware, previous.revision, exactRequest);
    if (!validated.succeeded) {
        finish(operationToken, {false, false, validated.error, std::nullopt});
        return operationToken;
    }

    m_operationActive = true;
    m_configurator.apply(
            hardware, previous.revision, validated.normalized,
            [this, operationToken, owner, hardware, previous, validated, timeoutMs,
             epoch = m_hardwareEpoch](ConfigurationResult result) mutable {
                if (!result.succeeded) {
                    finish(operationToken, std::move(result));
                    return;
                }
                const auto selected = result.selected
                        ? DisplayConfigurationValidator::validateExtended(
                                  hardware, previous.revision, *result.selected)
                        : validated;
                if (!selected.succeeded) {
                    finish(operationToken, {false, false, selected.error, std::nullopt});
                    return;
                }
                AppliedConfiguration applied = result.selected
                        ? makeApplied(hardware, selected, previous.revision + 1)
                        : result.applied.value_or(
                                  makeApplied(hardware, selected, previous.revision + 1));
                if (applied.revision <= previous.revision)
                    applied.revision = previous.revision + 1;
                if (timeoutMs < 0) {
                    DisplayError error;
                    if (!m_manager->installApplied(applied, &error))
                        finish(operationToken, {false, false, error, std::nullopt});
                    else {
                        result.applied = applied;
                        if (epoch != m_hardwareEpoch) {
                            result.succeeded = false;
                            result.error = {
                                    DisplayErrorCode::StaleConfiguration,
                                    QStringLiteral("Hardware changed during recovery publication"),
                                    {}};
                        }
                        finish(operationToken, std::move(result));
                    }
                    return;
                }
                Preview preview;
                preview.token = nextToken("preview");
                preview.owner = owner;
                preview.requested = validated.normalized;
                preview.rollbackApplied = previous;
                preview.rollbackRequest = makeRequest(hardware, previous);
                preview.deadlineMs = QDateTime::currentMSecsSinceEpoch() + std::max(1, timeoutMs);
                m_preview = std::move(preview);
                DisplayError error;
                if (!m_manager->installApplied(applied, &error)) {
                    m_operationActive = false;
                    startRevert(error.code, error.message, operationToken);
                    return;
                }
                if (epoch != m_hardwareEpoch) {
                    m_preview.reset();
                    finish(operationToken,
                           {false,
                            false,
                            {DisplayErrorCode::StaleConfiguration,
                             QStringLiteral("Hardware changed during publication"),
                             {}},
                            applied});
                    return;
                }
                if (timeoutMs == 0) {
                    if (!m_preferences.save(hardware, validated.normalized, &error)) {
                        m_operationActive = false;
                        startRevert(error.code, error.message, operationToken);
                        return;
                    }
                    m_preview.reset();
                    result.applied = applied;
                    finish(operationToken, std::move(result));
                    return;
                }
                m_previewTimer.start(std::max(1, timeoutMs));
                result.applied = applied;
                m_operationActive = false;
                emit previewChanged(m_preview->token, m_preview->deadlineMs);
                emit operationFinished(operationToken, std::move(result));
            });
    return operationToken;
}

void DisplayConfigurationController::confirm(const QString &token, const QString &owner)
{
    const QString operationToken = nextToken("confirm");
    if (!m_preview || m_preview->token != token || m_preview->owner != owner) {
        emit operationFinished(operationToken,
                               {false,
                                false,
                                {DisplayErrorCode::Cancelled,
                                 QStringLiteral("The preview token is not owned by this client"),
                                 {}},
                                std::nullopt});
        return;
    }
    DisplayError error;
    if (!m_manager->hardware()
        || !m_preferences.save(*m_manager->hardware(), m_preview->requested, &error)) {
        finish(operationToken, {false, false, error, std::nullopt});
        return;
    }
    m_previewTimer.stop();
    m_preview.reset();
    emit previewChanged({}, 0);
    finish(operationToken, {true, false, {}, m_manager->applied()});
}

void DisplayConfigurationController::revert(const QString &token, const QString &owner)
{
    if (!m_preview || m_preview->token != token || m_preview->owner != owner) {
        emit operationFinished(nextToken("revert"),
                               {false,
                                false,
                                {DisplayErrorCode::Cancelled,
                                 QStringLiteral("The preview token is not owned by this client"),
                                 {}},
                                std::nullopt});
        return;
    }
    startRevert(DisplayErrorCode::Cancelled, QStringLiteral("Display preview reverted"));
}

void DisplayConfigurationController::startRevert(DisplayErrorCode reason, QString message,
                                                 QString failedOperation)
{
    if (!m_preview || m_operationActive)
        return;
    m_previewTimer.stop();
    const QString operationToken =
            failedOperation.isEmpty() ? nextToken("revert") : failedOperation;
    Preview preview = *m_preview;
    m_operationActive = true;
    const auto hardware = m_manager->hardware();
    if (!hardware || hardware->topologyGeneration != preview.rollbackApplied.topologyGeneration) {
        m_preview.reset();
        emit previewChanged({}, 0);
        finish(operationToken,
               {false,
                false,
                {DisplayErrorCode::RevertFailed,
                 QStringLiteral("Display topology changed during preview"),
                 {}},
                std::nullopt});
        return;
    }
    const uint64_t revision = m_manager->applied() ? m_manager->applied()->revision : 0;
    preview.rollbackRequest.base.appliedRevision = revision;
    m_configurator.apply(
            *hardware, revision, preview.rollbackRequest,
            [this, operationToken, preview, reason, failedOperation,
             message = std::move(message)](ConfigurationResult result) mutable {
                m_preview.reset();
                emit previewChanged({}, 0);
                if (!result.succeeded) {
                    result.error.code = DisplayErrorCode::RevertFailed;
                    finish(operationToken, std::move(result));
                    return;
                }
                AppliedConfiguration restored = preview.rollbackApplied;
                restored.revision = m_manager->applied() ? m_manager->applied()->revision + 1
                                                         : restored.revision + 1;
                DisplayError publishError;
                if (!m_manager->installApplied(restored, &publishError)) {
                    finish(operationToken, {false, false, publishError, std::nullopt});
                    return;
                }
                finish(operationToken,
                       {failedOperation.isEmpty(), false, {reason, message, {}}, restored});
            });
}

QString DisplayConfigurationController::setPower(const QList<DisplayId> &displays,
                                                 NativeDisplayDelegate::PowerState state)
{
    const QString token = nextToken("power");
    if (m_operationActive || m_preview || m_manager->isPublishing() || m_topologyPending) {
        emit operationFinished(
                token,
                {false,
                 false,
                 {DisplayErrorCode::Busy, QStringLiteral("A display transaction is active"), {}},
                 std::nullopt});
        return token;
    }
    m_operationActive = true;
    m_delegate->setDisplayPower(displays, state, [this, token](ConfigurationResult result) mutable {
        finish(token, std::move(result));
    });
    return token;
}

void DisplayConfigurationController::ownerDisconnected(const QString &owner)
{
    if (m_preview && m_preview->owner == owner)
        startRevert(DisplayErrorCode::Cancelled, QStringLiteral("Display client disconnected"));
}

void DisplayConfigurationController::hardwareInvalidated(uint64_t)
{
    ++m_hardwareEpoch;
    m_previewTimer.stop();
    if (m_preview) {
        m_preview.reset();
        emit previewChanged({}, 0);
    }
}

QString DisplayConfigurationController::previewToken() const
{
    return m_preview ? m_preview->token : QString();
}

QString DisplayConfigurationController::previewOwner() const
{
    return m_preview ? m_preview->owner : QString();
}

AppliedConfiguration
DisplayConfigurationController::makeApplied(const HardwareSnapshot &hardware,
                                            const DisplayConfigurationValidator::Result &validated,
                                            uint64_t revision)
{
    AppliedConfiguration applied;
    applied.compositorInstanceId = hardware.compositorInstanceId;
    applied.topologyGeneration = hardware.topologyGeneration;
    applied.revision = revision;
    applied.physicalDisplays = hardware.displays;
    for (auto &physical : applied.physicalDisplays) {
        for (const auto &request : validated.normalized.displays) {
            if (request.display == physical.sessionId)
                physical.currentMode = request.enabled ? std::optional(request.mode) : std::nullopt;
        }
    }
    applied.placements = validated.normalized.placements;
    for (const auto &request : validated.normalized.displays) {
        if (!request.enabled)
            continue;
        bool destinationOnly = false;
        QList<DisplayId> destinations {request.display};
        for (const auto &group : validated.normalized.mirrorGroups) {
            if (group.first() == request.display)
                destinations = group;
            else if (group.mid(1).contains(request.display))
                destinationOnly = true;
        }
        if (destinationOnly)
            continue;
        const QRect bounds = validated.logicalBounds.value(request.display);
        const auto *physical = snapshotForId(hardware, request.display);
        applied.logicalDisplays.append(
                {request.display, destinations,
                 physical ? physical->connectorName : request.display.value(), bounds, bounds,
                 request.scale, request.rotation, request.display == validated.normalized.primary});
    }
    return applied;
}

DisplayConfiguration
DisplayConfigurationController::makeRequest(const HardwareSnapshot &hardware,
                                            const AppliedConfiguration &applied)
{
    DisplayConfiguration result;
    result.base = {hardware.compositorInstanceId, hardware.topologyGeneration, applied.revision};
    for (const auto &physical : applied.physicalDisplays) {
        const LogicalDisplay *logical = nullptr;
        for (const auto &candidate : applied.logicalDisplays) {
            if (candidate.destinations.contains(physical.sessionId)) {
                logical = &candidate;
                break;
            }
        }
        const auto mode = physical.currentMode ? physical.currentMode : physical.nativeMode;
        if (!mode)
            continue;
        result.displays.append({physical.sessionId, logical != nullptr, *mode,
                                logical ? logical->scale : compositor::OutputScale(),
                                logical ? logical->rotation : DisplayRotation::Normal});
        if (logical && logical->primary)
            result.primary = logical->destinations.first();
    }
    result.placements = applied.placements;
    for (const auto &logical : applied.logicalDisplays) {
        if (logical.destinations.size() > 1)
            result.mirrorGroups.append(logical.destinations);
    }
    return result;
}

} // namespace cutefish::display
