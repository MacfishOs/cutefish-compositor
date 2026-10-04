// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/cros_display_config.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: a credential-checked session D-Bus object fronts the
// compositor transaction controller without Ash or Mojo.

#pragma once

#include <chrono>
#include <functional>

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QObject>

#include "display/display_configuration_controller.h"
#include "ipc/display_dbus_types.h"

namespace cutefish::display::ipc {

class DisplayConfigService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.DisplayConfig1")
    Q_PROPERTY(uint ProtocolVersion READ protocolVersion CONSTANT)

public:
    using SessionActive = std::function<bool()>;

    explicit DisplayConfigService(DisplayManager* manager,
                                  DisplayConfigurationController* controller,
                                  QDBusConnection connection = QDBusConnection::sessionBus(),
                                  SessionActive sessionActive = [] { return true; },
                                  QObject* parent = nullptr);

    uint protocolVersion() const { return 1; }
    bool registerService(const QString& serviceName);

    // Receives SetNightLight's strength, 0 to 1, and how long to take to it.
    using NightLightHandler = std::function<void(double, std::chrono::milliseconds)>;

    void setNightLightHandler(NightLightHandler handler) { m_nightLight = std::move(handler); }

public slots:
    QString GetState(QString& seatId, qulonglong& topologyGeneration,
                     qulonglong& appliedRevision, QList<PhysicalDisplay>& physicalDisplays,
                     QList<LogicalDisplay>& logicalDisplays, QList<Placement>& placements,
                     QString& previewToken, qulonglong& previewDeadlineMs);
    QString TestConfiguration(const QString& instanceId, qulonglong topologyGeneration,
                              qulonglong appliedRevision,
                              const QList<DisplayRequest>& displays, const QString& primary,
                              const QList<Placement>& placements,
                              const MirrorGroups& mirrorGroups);
    QString ApplyConfiguration(const QString& instanceId, qulonglong topologyGeneration,
                               qulonglong appliedRevision,
                               const QList<DisplayRequest>& displays, const QString& primary,
                               const QList<Placement>& placements,
                               const MirrorGroups& mirrorGroups);
    QString SetConfiguration(const QString& instanceId, qulonglong topologyGeneration,
                             qulonglong appliedRevision,
                             const QList<DisplayRequest>& displays, const QString& primary,
                             const QList<Placement>& placements,
                             const MirrorGroups& mirrorGroups);
    void ConfirmConfiguration(const QString& previewToken);
    void RevertConfiguration(const QString& previewToken);
    QString ResetConfiguration(const QString& instanceId, qulonglong topologyGeneration,
                               qulonglong appliedRevision);
    QString SetDisplayPower(const QStringList& displayIds, uint state);
    QString SetDockedLidClosed(bool closed);
    void SetNightLight(double strength, uint durationMs);

signals:
    void StateChanged(qulonglong topologyGeneration, qulonglong appliedRevision);
    void OperationFinished(QString operationToken, bool succeeded, bool degraded, uint errorCode,
                           QString errorMessage, QStringList affectedDisplayIds,
                           qulonglong topologyGeneration, qulonglong appliedRevision);
    void PreviewChanged(QString previewToken, qulonglong deadlineMs,
                        qulonglong topologyGeneration, qulonglong appliedRevision);

private:
    bool authorizeWrite();
    // The caller owns this session, whether or not it is on screen.
    bool authorizeCaller();
    QString caller() const;
    QString reject(DisplayErrorCode code, const QString& message);
    DisplayConfiguration defaultConfiguration(const QString& instanceId,
                                              qulonglong topologyGeneration,
                                              qulonglong appliedRevision) const;

    DisplayManager* m_manager = nullptr;
    DisplayConfigurationController* m_controller = nullptr;
    QDBusConnection m_connection;
    SessionActive m_sessionActive;
    NightLightHandler m_nightLight;
    QString m_serviceName;
};

} // namespace cutefish::display::ipc
