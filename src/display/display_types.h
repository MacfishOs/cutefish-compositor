// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/types/display_snapshot.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: hardware, requested, applied, and persistent-facing
// identities are separate Qt value types with no Chromium platform graph.

#pragma once

#include <cstdint>
#include <optional>

#include <QFlags>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include "compositor/output_scale.h"

namespace cutefish::display {

class DisplayId
{
public:
    DisplayId() = default;

    explicit DisplayId(QString value)
        : m_value(std::move(value))
    {
    }

    bool isValid() const { return !m_value.isEmpty(); }

    const QString &value() const { return m_value; }

    friend bool operator==(const DisplayId &, const DisplayId &) = default;

    friend bool operator<(const DisplayId &left, const DisplayId &right)
    {
        return left.m_value < right.m_value;
    }

private:
    QString m_value;
};

size_t qHash(const DisplayId &id, size_t seed = 0) noexcept;

enum class DisplayRotation : uint32_t {
    Normal = 0,
    Rotate90 = 1,
    Rotate180 = 2,
    Rotate270 = 3,
};

// D-Bus and the preferences file carry these as plain integers.
inline bool isValid(DisplayRotation rotation)
{
    return uint32_t(rotation) <= uint32_t(DisplayRotation::Rotate270);
}

enum class DisplayConnectionType : uint32_t {
    Unknown,
    Internal,
    External,
    Virtual,
};

enum class DisplayCapability : uint32_t {
    None = 0,
    Scale = 1 << 0,
    Rotation = 1 << 1,
    SoftwareMirror = 1 << 2,
    Power = 1 << 3,
    VariableRefreshRate = 1 << 4,
    HighDynamicRange = 1 << 5,
    PrivacyScreen = 1 << 6,
};
Q_DECLARE_FLAGS(DisplayCapabilities, DisplayCapability)

struct DisplayMode {
    QSize pixelSize;
    int refreshMilliHz = 0;
    int pixelClockKHz = 0;
    int horizontalTotal = 0;
    int verticalTotal = 0;
    uint32_t flags = 0;
    QString name;
    bool preferred = false;

    bool isValid() const;
    QString stableKey() const;
    friend bool operator==(const DisplayMode &, const DisplayMode &) = default;
};

struct DisplaySnapshot {
    DisplayId sessionId;
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
    QSize physicalSizeMm;
    DisplayConnectionType connectionType = DisplayConnectionType::Unknown;
    DisplayRotation panelOrientation = DisplayRotation::Normal;
    DisplayCapabilities capabilities;
    QList<DisplayMode> modes;
    std::optional<DisplayMode> currentMode;
    std::optional<DisplayMode> nativeMode;

    bool isValid() const;
};

struct HardwareSnapshot {
    QString compositorInstanceId;
    QString seatId;
    uint64_t topologyGeneration = 0;
    QList<DisplaySnapshot> displays;

    bool isValid() const;
};

enum class LayoutEdge : uint32_t {
    Top,
    Right,
    Bottom,
    Left,
};

inline bool isValid(LayoutEdge edge)
{
    return uint32_t(edge) <= uint32_t(LayoutEdge::Left);
}

struct DisplayPlacement {
    DisplayId display;
    DisplayId parent;
    LayoutEdge edge = LayoutEdge::Right;
    int offset = 0;

    friend bool operator==(const DisplayPlacement &, const DisplayPlacement &) = default;
};

struct DisplayConfigurationBase {
    QString compositorInstanceId;
    uint64_t topologyGeneration = 0;
    uint64_t appliedRevision = 0;
};

struct DisplayRequest {
    DisplayId display;
    bool enabled = true;
    DisplayMode mode;
    compositor::OutputScale scale;
    DisplayRotation rotation = DisplayRotation::Normal;
};

struct DisplayConfiguration {
    DisplayConfigurationBase base;
    QList<DisplayRequest> displays;
    DisplayId primary;
    QList<DisplayPlacement> placements;
    QList<QList<DisplayId>> mirrorGroups;
    // Internal recovery policy; never accepted from IPC or persisted.
    bool allowModeFallback = false;
};

struct LogicalDisplay {
    DisplayId id;
    QList<DisplayId> destinations;
    QString waylandName;
    QRect bounds;
    QRect workArea;
    compositor::OutputScale scale;
    DisplayRotation rotation = DisplayRotation::Normal;
    bool primary = false;
};

struct AppliedConfiguration {
    QString compositorInstanceId;
    uint64_t topologyGeneration = 0;
    uint64_t revision = 0;
    QList<DisplaySnapshot> physicalDisplays;
    QList<LogicalDisplay> logicalDisplays;
    QList<DisplayPlacement> placements;
};

enum class DisplayErrorCode : uint32_t {
    None,
    Unavailable,
    Unauthorized,
    Busy,
    StaleConfiguration,
    UnknownOutput,
    UnsupportedMode,
    UnsupportedProperty,
    InvalidLayout,
    NoEnabledOutput,
    ResourceExhausted,
    TestFailed,
    CommitFailed,
    Cancelled,
    RevertFailed,
    PersistenceFailed,
};

struct DisplayError {
    DisplayErrorCode code = DisplayErrorCode::None;
    QString message;
    QList<DisplayId> affectedDisplays;
};

struct ConfigurationResult {
    bool succeeded = false;
    bool degraded = false;
    DisplayError error;
    std::optional<AppliedConfiguration> applied;
    // The successful test candidate is committed exactly, then used for publication.
    std::optional<DisplayConfiguration> selected;
};

} // namespace cutefish::display

Q_DECLARE_OPERATORS_FOR_FLAGS(cutefish::display::DisplayCapabilities)
