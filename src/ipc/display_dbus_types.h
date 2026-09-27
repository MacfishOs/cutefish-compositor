// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/cros_display_config.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: fixed Qt D-Bus tuples replace Ash service and Mojo
// types while preserving capability-rich display descriptions.

#pragma once

#include <QDBusArgument>
#include <QList>
#include <QString>
#include <QStringList>

#include "display/display_types.h"

namespace cutefish::display::ipc {

struct Mode {
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    int pixelClockKHz = 0;
    int horizontalTotal = 0;
    int verticalTotal = 0;
    uint flags = 0;
    bool preferred = false;
    bool current = false;
    QString name;
};

struct RequestedMode {
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    int pixelClockKHz = 0;
    int horizontalTotal = 0;
    int verticalTotal = 0;
    uint flags = 0;
    bool preferred = false;
    QString name;
};

struct PhysicalDisplay {
    QString sessionId;
    QString persistentKey;
    QString portKey;
    QString edidKey;
    QString connectorName;
    QString devicePath;
    QString baseConnectorKey;
    QStringList pathTopology;
    QString make;
    QString model;
    QString serialNumber;
    int physicalWidthMm = 0;
    int physicalHeightMm = 0;
    uint connectionType = 0;
    uint panelOrientation = 0;
    uint capabilities = 0;
    uint recommendedScale120 = 120;
    QList<Mode> modes;
};

struct LogicalDisplay {
    QString id;
    QString waylandName;
    QStringList destinations;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int workX = 0;
    int workY = 0;
    int workWidth = 0;
    int workHeight = 0;
    uint scale120 = 120;
    uint rotation = 0;
    bool primary = false;
};

struct DisplayRequest {
    QString id;
    bool enabled = true;
    RequestedMode mode;
    uint scale120 = 120;
    uint rotation = 0;
};

struct Placement {
    QString displayId;
    QString parentId;
    uint edge = 0;
    int offset = 0;
};

using MirrorGroups = QList<QStringList>;

QDBusArgument& operator<<(QDBusArgument& argument, const Mode& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, Mode& value);
QDBusArgument& operator<<(QDBusArgument& argument, const RequestedMode& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, RequestedMode& value);
QDBusArgument& operator<<(QDBusArgument& argument, const PhysicalDisplay& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, PhysicalDisplay& value);
QDBusArgument& operator<<(QDBusArgument& argument, const LogicalDisplay& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, LogicalDisplay& value);
QDBusArgument& operator<<(QDBusArgument& argument, const DisplayRequest& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, DisplayRequest& value);
QDBusArgument& operator<<(QDBusArgument& argument, const Placement& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, Placement& value);

void registerTypes();
DisplayConfiguration fromWire(const QString& instanceId, qulonglong topologyGeneration,
                              qulonglong appliedRevision, const QList<DisplayRequest>& displays,
                              const QString& primary, const QList<Placement>& placements,
                              const MirrorGroups& mirrorGroups);
QList<PhysicalDisplay> toWire(const QList<display::DisplaySnapshot>& displays);
QList<LogicalDisplay> toWire(const QList<display::LogicalDisplay>& displays);
QList<Placement> toWire(const QList<display::DisplayPlacement>& placements);

} // namespace cutefish::display::ipc

Q_DECLARE_METATYPE(cutefish::display::ipc::Mode)
Q_DECLARE_METATYPE(cutefish::display::ipc::RequestedMode)
Q_DECLARE_METATYPE(cutefish::display::ipc::PhysicalDisplay)
Q_DECLARE_METATYPE(cutefish::display::ipc::LogicalDisplay)
Q_DECLARE_METATYPE(cutefish::display::ipc::DisplayRequest)
Q_DECLARE_METATYPE(cutefish::display::ipc::Placement)
Q_DECLARE_METATYPE(QList<cutefish::display::ipc::Mode>)
Q_DECLARE_METATYPE(QList<cutefish::display::ipc::PhysicalDisplay>)
Q_DECLARE_METATYPE(QList<cutefish::display::ipc::LogicalDisplay>)
Q_DECLARE_METATYPE(QList<cutefish::display::ipc::DisplayRequest>)
Q_DECLARE_METATYPE(QList<cutefish::display::ipc::Placement>)
Q_DECLARE_METATYPE(cutefish::display::ipc::MirrorGroups)
