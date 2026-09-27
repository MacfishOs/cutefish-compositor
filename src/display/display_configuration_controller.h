// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/resolution_notification_controller.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: every user display change is a compositor-owned
// preview transaction that confirms, times out, or restores prior state.

#pragma once

#include <optional>

#include <QObject>
#include <QTimer>

#include "display/display_configurator.h"
#include "display/display_manager.h"
#include "display/display_preferences.h"

namespace cutefish::display {

class DisplayConfigurationController : public QObject
{
    Q_OBJECT

public:
    explicit DisplayConfigurationController(DisplayManager *manager,
                                            NativeDisplayDelegate *delegate,
                                            DisplayPreferences preferences = DisplayPreferences(),
                                            QObject *parent = nullptr);

    QString test(const DisplayConfiguration &request);
    // A zero timeout commits and persists immediately, without a preview.
    QString apply(const DisplayConfiguration &request, const QString &owner, int timeoutMs = 15000);
    void confirm(const QString &previewToken, const QString &owner);
    void revert(const QString &previewToken, const QString &owner);
    QString setPower(const QList<DisplayId> &displays, NativeDisplayDelegate::PowerState state);
    void ownerDisconnected(const QString &owner);
    void hardwareInvalidated(uint64_t generation);

    std::optional<AppliedConfiguration> recoveryState() const
    {
        return m_preview ? std::optional(m_preview->rollbackApplied) : m_manager->applied();
    }

    void setTopologyPending(bool pending) { m_topologyPending = pending; }

    bool busy() const { return m_operationActive; }

    QString previewToken() const;

    qint64 previewDeadlineMs() const { return m_preview ? m_preview->deadlineMs : 0; }

    QString previewOwner() const;

signals:
    void operationFinished(QString operationToken, cutefish::display::ConfigurationResult result);
    void previewChanged(QString previewToken, qint64 deadlineMs);

private:
    struct Preview {
        QString token;
        QString owner;
        DisplayConfiguration requested;
        DisplayConfiguration rollbackRequest;
        AppliedConfiguration rollbackApplied;
        qint64 deadlineMs = 0;
    };

    static AppliedConfiguration makeApplied(const HardwareSnapshot &hardware,
                                            const DisplayConfigurationValidator::Result &validated,
                                            uint64_t revision);
    static DisplayConfiguration makeRequest(const HardwareSnapshot &hardware,
                                            const AppliedConfiguration &applied);
    QString nextToken(const char *prefix) const;
    void finish(const QString &operationToken, ConfigurationResult result);
    void startRevert(DisplayErrorCode reason, QString message, QString failedOperation = {});

    DisplayManager *m_manager = nullptr;
    NativeDisplayDelegate *m_delegate = nullptr;
    DisplayConfigurator m_configurator;
    DisplayPreferences m_preferences;
    QTimer m_previewTimer;
    std::optional<Preview> m_preview;
    bool m_operationActive = false;
    bool m_topologyPending = false;
    uint64_t m_hardwareEpoch = 0;
};

} // namespace cutefish::display

Q_DECLARE_METATYPE(cutefish::display::ConfigurationResult)
