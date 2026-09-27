// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/cros_display_config.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: serialize the native display contract through fixed
// session D-Bus structures without Ash or Mojo dependencies.

#include "ipc/display_dbus_types.h"

#include <QDBusMetaType>

#include "display/recommended_scale.h"

namespace cutefish::display::ipc {

#define CUTEFISH_DBUS_WRITE(Type, Fields) \
    QDBusArgument& operator<<(QDBusArgument& argument, const Type& value) \
    { \
        argument.beginStructure(); \
        argument Fields; \
        argument.endStructure(); \
        return argument; \
    }

#define CUTEFISH_DBUS_READ(Type, Fields) \
    const QDBusArgument& operator>>(const QDBusArgument& argument, Type& value) \
    { \
        argument.beginStructure(); \
        argument Fields; \
        argument.endStructure(); \
        return argument; \
    }

CUTEFISH_DBUS_WRITE(Mode, << value.width << value.height << value.refreshMilliHz
                           << value.pixelClockKHz << value.horizontalTotal << value.verticalTotal
                           << value.flags << value.preferred << value.current << value.name)
CUTEFISH_DBUS_READ(Mode, >> value.width >> value.height >> value.refreshMilliHz
                          >> value.pixelClockKHz >> value.horizontalTotal >> value.verticalTotal
                          >> value.flags >> value.preferred >> value.current >> value.name)
CUTEFISH_DBUS_WRITE(RequestedMode, << value.width << value.height << value.refreshMilliHz
                                    << value.pixelClockKHz << value.horizontalTotal
                                    << value.verticalTotal << value.flags << value.preferred
                                    << value.name)
CUTEFISH_DBUS_READ(RequestedMode, >> value.width >> value.height >> value.refreshMilliHz
                                   >> value.pixelClockKHz >> value.horizontalTotal
                                   >> value.verticalTotal >> value.flags >> value.preferred
                                   >> value.name)
CUTEFISH_DBUS_WRITE(PhysicalDisplay, << value.sessionId << value.persistentKey << value.portKey
                                     << value.edidKey << value.connectorName << value.devicePath
                                     << value.baseConnectorKey << value.pathTopology << value.make
                                     << value.model << value.serialNumber << value.physicalWidthMm
                                     << value.physicalHeightMm << value.connectionType
                                     << value.panelOrientation << value.capabilities
                                     << value.recommendedScale120 << value.modes)
CUTEFISH_DBUS_READ(PhysicalDisplay, >> value.sessionId >> value.persistentKey >> value.portKey
                                    >> value.edidKey >> value.connectorName >> value.devicePath
                                    >> value.baseConnectorKey >> value.pathTopology >> value.make
                                    >> value.model >> value.serialNumber >> value.physicalWidthMm
                                    >> value.physicalHeightMm >> value.connectionType
                                    >> value.panelOrientation >> value.capabilities
                                    >> value.recommendedScale120 >> value.modes)
CUTEFISH_DBUS_WRITE(LogicalDisplay, << value.id << value.waylandName << value.destinations
                                    << value.x << value.y << value.width << value.height
                                    << value.workX << value.workY << value.workWidth << value.workHeight
                                    << value.scale120 << value.rotation << value.primary)
CUTEFISH_DBUS_READ(LogicalDisplay, >> value.id >> value.waylandName >> value.destinations
                                   >> value.x >> value.y >> value.width >> value.height
                                   >> value.workX >> value.workY >> value.workWidth >> value.workHeight
                                   >> value.scale120 >> value.rotation >> value.primary)
CUTEFISH_DBUS_WRITE(DisplayRequest, << value.id << value.enabled << value.mode << value.scale120
                                    << value.rotation)
CUTEFISH_DBUS_READ(DisplayRequest, >> value.id >> value.enabled >> value.mode >> value.scale120
                                   >> value.rotation)
CUTEFISH_DBUS_WRITE(Placement, << value.displayId << value.parentId << value.edge << value.offset)
CUTEFISH_DBUS_READ(Placement, >> value.displayId >> value.parentId >> value.edge >> value.offset)

#undef CUTEFISH_DBUS_WRITE
#undef CUTEFISH_DBUS_READ

void registerTypes()
{
    qDBusRegisterMetaType<Mode>();
    qDBusRegisterMetaType<RequestedMode>();
    qDBusRegisterMetaType<PhysicalDisplay>();
    qDBusRegisterMetaType<LogicalDisplay>();
    qDBusRegisterMetaType<DisplayRequest>();
    qDBusRegisterMetaType<Placement>();
    qDBusRegisterMetaType<QList<Mode>>();
    qDBusRegisterMetaType<QList<PhysicalDisplay>>();
    qDBusRegisterMetaType<QList<LogicalDisplay>>();
    qDBusRegisterMetaType<QList<DisplayRequest>>();
    qDBusRegisterMetaType<QList<Placement>>();
    qDBusRegisterMetaType<MirrorGroups>();
}

DisplayConfiguration fromWire(const QString& instanceId, qulonglong topologyGeneration,
                              qulonglong appliedRevision, const QList<DisplayRequest>& displays,
                              const QString& primary, const QList<Placement>& placements,
                              const MirrorGroups& mirrorGroups)
{
    display::DisplayConfiguration result;
    result.base = {instanceId, topologyGeneration, appliedRevision};
    result.primary = display::DisplayId(primary);
    for (const auto& value : displays) {
        display::DisplayMode mode;
        mode.pixelSize = {value.mode.width, value.mode.height};
        mode.refreshMilliHz = value.mode.refreshMilliHz;
        mode.pixelClockKHz = value.mode.pixelClockKHz;
        mode.horizontalTotal = value.mode.horizontalTotal;
        mode.verticalTotal = value.mode.verticalTotal;
        mode.flags = value.mode.flags;
        mode.preferred = value.mode.preferred;
        mode.name = value.mode.name;
        const auto scale = compositor::OutputScale::fromNumerator(value.scale120);
        result.displays.append({display::DisplayId(value.id), value.enabled, mode,
                                scale.value_or(compositor::OutputScale()),
                                display::DisplayRotation(value.rotation)});
    }
    for (const auto& value : placements) {
        result.placements.append({display::DisplayId(value.displayId),
                                  display::DisplayId(value.parentId),
                                  display::LayoutEdge(value.edge), value.offset});
    }
    for (const auto& group : mirrorGroups) {
        QList<display::DisplayId> ids;
        for (const auto& value : group)
            ids.append(display::DisplayId(value));
        result.mirrorGroups.append(ids);
    }
    return result;
}

QList<PhysicalDisplay> toWire(const QList<display::DisplaySnapshot>& displays)
{
    QList<PhysicalDisplay> result;
    for (const auto& value : displays) {
        PhysicalDisplay item;
        item.sessionId = value.sessionId.value();
        item.persistentKey = value.persistentKey;
        item.portKey = value.portKey;
        item.edidKey = value.edidKey;
        item.connectorName = value.connectorName;
        item.devicePath = value.devicePath;
        item.baseConnectorKey = value.baseConnectorKey;
        item.pathTopology = value.pathTopology;
        item.make = value.make;
        item.model = value.model;
        item.serialNumber = value.serialNumber;
        item.physicalWidthMm = value.physicalSizeMm.width();
        item.physicalHeightMm = value.physicalSizeMm.height();
        item.connectionType = uint(value.connectionType);
        item.panelOrientation = uint(value.panelOrientation);
        item.capabilities = uint(value.capabilities.toInt());
        item.recommendedScale120 = recommendedScale(value).numerator();
        for (const auto& mode : value.modes) {
            item.modes.append({mode.pixelSize.width(), mode.pixelSize.height(),
                               mode.refreshMilliHz, mode.pixelClockKHz, mode.horizontalTotal,
                               mode.verticalTotal, mode.flags, mode.preferred,
                               value.currentMode && value.currentMode->stableKey() == mode.stableKey(),
                               mode.name});
        }
        result.append(item);
    }
    return result;
}

QList<LogicalDisplay> toWire(const QList<display::LogicalDisplay>& displays)
{
    QList<LogicalDisplay> result;
    for (const auto& value : displays) {
        QStringList destinations;
        for (const auto& id : value.destinations)
            destinations.append(id.value());
        result.append({value.id.value(), value.waylandName, destinations,
                       value.bounds.x(), value.bounds.y(), value.bounds.width(), value.bounds.height(),
                       value.workArea.x(), value.workArea.y(), value.workArea.width(),
                       value.workArea.height(), value.scale.numerator(), uint(value.rotation),
                       value.primary});
    }
    return result;
}

QList<Placement> toWire(const QList<display::DisplayPlacement>& placements)
{
    QList<Placement> result;
    for (const auto& value : placements)
        result.append({value.display.value(), value.parent.value(), uint(value.edge), value.offset});
    return result;
}

} // namespace cutefish::display::ipc
