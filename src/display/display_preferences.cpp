// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/display_prefs.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: QSaveFile and stable monitor keys replace Chrome's
// preference service while preserving confirmed-only persistence.

#include "display/display_preferences.h"
#include "display/recommended_scale.h"

#include <algorithm>
#include <functional>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace cutefish::display {
namespace {

constexpr int kSchemaVersion = 2;
constexpr qint64 kMaximumFileSize = 1024 * 1024;

void fail(DisplayError *error, DisplayErrorCode code, const QString &message)
{
    if (error)
        *error = {code, message, {}};
}

const DisplaySnapshot *snapshotForId(const HardwareSnapshot &hardware, const DisplayId &id)
{
    for (const auto &display : hardware.displays) {
        if (display.sessionId == id)
            return &display;
    }
    return nullptr;
}

const DisplaySnapshot *snapshotForPersistentKey(const HardwareSnapshot &hardware,
                                                const QString &key)
{
    const DisplaySnapshot *found = nullptr;
    for (const auto &display : hardware.displays) {
        if (display.persistentKey != key)
            continue;
        if (found)
            return nullptr;
        found = &display;
    }
    return found;
}

QJsonObject modeToJson(const DisplayMode &mode)
{
    return {{QStringLiteral("width"), mode.pixelSize.width()},
            {QStringLiteral("height"), mode.pixelSize.height()},
            {QStringLiteral("refreshMilliHz"), mode.refreshMilliHz},
            {QStringLiteral("pixelClockKHz"), mode.pixelClockKHz},
            {QStringLiteral("horizontalTotal"), mode.horizontalTotal},
            {QStringLiteral("verticalTotal"), mode.verticalTotal},
            {QStringLiteral("flags"), int(mode.flags)},
            {QStringLiteral("name"), mode.name}};
}

std::optional<DisplayMode> modeFromJson(const DisplaySnapshot &display, const QJsonObject &object)
{
    DisplayMode requested;
    requested.pixelSize = {object.value(QStringLiteral("width")).toInt(),
                           object.value(QStringLiteral("height")).toInt()};
    requested.refreshMilliHz = object.value(QStringLiteral("refreshMilliHz")).toInt();
    requested.pixelClockKHz = object.value(QStringLiteral("pixelClockKHz")).toInt();
    requested.horizontalTotal = object.value(QStringLiteral("horizontalTotal")).toInt();
    requested.verticalTotal = object.value(QStringLiteral("verticalTotal")).toInt();
    requested.flags = uint32_t(object.value(QStringLiteral("flags")).toInt());
    requested.name = object.value(QStringLiteral("name")).toString();
    for (const auto &mode : display.modes) {
        if (mode.stableKey() == requested.stableKey())
            return mode;
    }
    return std::nullopt;
}

QString keyForId(const HardwareSnapshot &hardware, const DisplayId &id)
{
    const auto *display = snapshotForId(hardware, id);
    return display ? display->persistentKey : QString();
}

} // namespace

DisplayPreferences::DisplayPreferences(QString path)
    : m_path(path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                             + QStringLiteral("/cutefish/display.json")
                            : std::move(path))
{
}

QStringList DisplayPreferences::persistentKeys() const
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumFileSize)
        return {};
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != kSchemaVersion)
        return {};
    return root.value(QStringLiteral("monitors")).toObject().keys();
}

QString DisplayPreferences::topologyKey(const HardwareSnapshot &hardware)
{
    QStringList keys;
    for (const auto &display : hardware.displays)
        keys.append(display.persistentKey);
    std::sort(keys.begin(), keys.end());
    return keys.join(QLatin1Char('|'));
}

bool DisplayPreferences::save(const HardwareSnapshot &hardware,
                              const DisplayConfiguration &configuration, DisplayError *error) const
{
    QJsonObject monitors;
    QJsonArray enabled;
    for (const auto &request : configuration.displays) {
        const auto *display = snapshotForId(hardware, request.display);
        if (!display)
            continue;
        monitors.insert(display->persistentKey,
                        QJsonObject {{QStringLiteral("preferredPort"), display->portKey},
                                     {QStringLiteral("mode"), modeToJson(request.mode)},
                                     {QStringLiteral("scale120"), int(request.scale.numerator())},
                                     {QStringLiteral("rotation"), int(request.rotation)}});
        if (request.enabled)
            enabled.append(display->persistentKey);
    }

    QJsonArray placements;
    for (const auto &placement : configuration.placements) {
        placements.append(
                QJsonObject {{QStringLiteral("display"), keyForId(hardware, placement.display)},
                             {QStringLiteral("parent"), keyForId(hardware, placement.parent)},
                             {QStringLiteral("edge"), int(placement.edge)},
                             {QStringLiteral("offset"), placement.offset}});
    }
    QJsonArray mirrorGroups;
    for (const auto &group : configuration.mirrorGroups) {
        QJsonArray stored;
        for (const auto &id : group)
            stored.append(keyForId(hardware, id));
        mirrorGroups.append(stored);
    }
    const QJsonObject profile {
            {QStringLiteral("primary"), keyForId(hardware, configuration.primary)},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("placements"), placements},
            {QStringLiteral("mirrorGroups"), mirrorGroups}};
    QJsonObject previous;
    QFile existing(m_path);
    if (existing.open(QIODevice::ReadOnly) && existing.size() <= kMaximumFileSize) {
        const auto stored = QJsonDocument::fromJson(existing.readAll()).object();
        if (stored.value(QStringLiteral("version")).toInt() == kSchemaVersion)
            previous = stored;
    }
    auto mergedMonitors = previous.value(QStringLiteral("monitors")).toObject();
    for (auto it = monitors.begin(); it != monitors.end(); ++it)
        mergedMonitors.insert(it.key(), it.value());
    auto topologies = previous.value(QStringLiteral("topologies")).toObject();
    topologies.insert(topologyKey(hardware), profile);
    const QJsonObject root {{QStringLiteral("version"), kSchemaVersion},
                            {QStringLiteral("monitors"), mergedMonitors},
                            {QStringLiteral("topologies"), topologies}};

    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        fail(error, DisplayErrorCode::PersistenceFailed,
             QStringLiteral("Could not create the display preference directory"));
        return false;
    }
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0
        || !file.commit()) {
        fail(error, DisplayErrorCode::PersistenceFailed,
             QStringLiteral("Could not atomically save display preferences"));
        return false;
    }
    return true;
}

std::optional<DisplayConfiguration> DisplayPreferences::restore(const HardwareSnapshot &hardware,
                                                                uint64_t appliedRevision,
                                                                DisplayError *error) const
{
    QFile file(m_path);
    if (!file.exists())
        return std::nullopt;
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumFileSize) {
        fail(error, DisplayErrorCode::PersistenceFailed,
             QStringLiteral("Display preferences cannot be read safely"));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto root = document.object();
    if (parseError.error != QJsonParseError::NoError
        || root.value(QStringLiteral("version")).toInt() != kSchemaVersion) {
        fail(error, DisplayErrorCode::PersistenceFailed,
             QStringLiteral("Display preferences have an unsupported schema"));
        return std::nullopt;
    }
    auto profile = root.value(QStringLiteral("topologies"))
                           .toObject()
                           .value(topologyKey(hardware))
                           .toObject();
    auto monitors = root.value(QStringLiteral("monitors")).toObject();
    if (profile.isEmpty()) {
        QStringList oldTopology;
        QHash<QString, QString> aliases;
        for (const auto &display : hardware.displays) {
            const auto oldPort = display.devicePath + QLatin1Char('/')
                    + display.portKey.section(QLatin1Char('/'), -1);
            QString oldKey = display.persistentKey;
            if (oldKey.startsWith(QStringLiteral("port:")))
                oldKey = QStringLiteral("port:") + oldPort;
            else if (oldKey.startsWith(QStringLiteral("virtual:")))
                oldKey = QStringLiteral("virtual:") + oldPort;
            else if (oldKey.contains(QLatin1Char('@')))
                oldKey = oldKey.section(QLatin1Char('@'), 0, 0) + QLatin1Char('@') + oldPort;
            aliases.insert(oldKey, display.persistentKey);
            oldTopology.append(oldKey);
            if (monitors.contains(oldKey))
                monitors.insert(display.persistentKey, monitors.value(oldKey));
        }
        std::sort(oldTopology.begin(), oldTopology.end());
        profile = root.value(QStringLiteral("topologies"))
                          .toObject()
                          .value(oldTopology.join(QLatin1Char('|')))
                          .toObject();
        if (profile.isEmpty()) {
            aliases.clear();
            oldTopology.clear();
            for (const auto &display : hardware.displays) {
                const QString oldKey = QStringLiteral("session:") + display.connectorName;
                aliases.insert(oldKey, display.persistentKey);
                oldTopology.append(oldKey + QStringLiteral("@port:") + display.connectorName);
                if (monitors.contains(oldKey))
                    monitors.insert(display.persistentKey, monitors.value(oldKey));
            }
            std::sort(oldTopology.begin(), oldTopology.end());
            profile = root.value(QStringLiteral("topologies"))
                              .toObject()
                              .value(oldTopology.join(QLatin1Char('|')))
                              .toObject();
        }
        std::function<QJsonValue(const QJsonValue &)> translate =
                [&](const QJsonValue &value) -> QJsonValue {
            if (value.isString())
                return aliases.value(value.toString(), value.toString());
            if (value.isArray()) {
                QJsonArray result;
                for (const auto &item : value.toArray())
                    result.append(translate(item));
                return result;
            }
            if (value.isObject()) {
                auto result = value.toObject();
                for (auto it = result.begin(); it != result.end(); ++it)
                    it.value() = translate(it.value());
                return result;
            }
            return value;
        };
        profile = translate(profile).toObject();
    }
    if (profile.isEmpty())
        return std::nullopt;

    DisplayConfiguration result;
    result.base = {hardware.compositorInstanceId, hardware.topologyGeneration, appliedRevision};
    QSet<QString> enabled;
    for (const auto &value : profile.value(QStringLiteral("enabled")).toArray())
        enabled.insert(value.toString());
    for (const auto &display : hardware.displays) {
        const auto saved = monitors.value(display.persistentKey).toObject();
        auto mode = modeFromJson(display, saved.value(QStringLiteral("mode")).toObject());
        if (!mode)
            mode = display.nativeMode ? display.nativeMode : display.currentMode;
        if (!mode)
            continue;
        const int scale120 = saved.value(QStringLiteral("scale120"))
                                     .toInt(int(recommendedScale(display).numerator()));
        const auto scale = compositor::OutputScale::fromNumerator(scale120);
        if (!scale)
            continue;
        // A value this build does not know falls back rather than failing the profile.
        auto rotation = DisplayRotation(saved.value(QStringLiteral("rotation")).toInt());
        if (!isValid(rotation))
            rotation = DisplayRotation::Normal;
        result.displays.append({display.sessionId, enabled.contains(display.persistentKey), *mode,
                                *scale, rotation});
    }
    const auto *primary =
            snapshotForPersistentKey(hardware, profile.value(QStringLiteral("primary")).toString());
    if (primary)
        result.primary = primary->sessionId;
    for (const auto &value : profile.value(QStringLiteral("placements")).toArray()) {
        const auto item = value.toObject();
        const auto *display = snapshotForPersistentKey(hardware, item.value("display").toString());
        const auto *parent = snapshotForPersistentKey(hardware, item.value("parent").toString());
        const auto edge = LayoutEdge(item.value("edge").toInt());
        if (display && parent && isValid(edge))
            result.placements.append({display->sessionId, parent->sessionId, edge,
                                      item.value("offset").toInt()});
    }
    for (const auto &value : profile.value(QStringLiteral("mirrorGroups")).toArray()) {
        QList<DisplayId> group;
        for (const auto &key : value.toArray()) {
            if (const auto *display = snapshotForPersistentKey(hardware, key.toString()))
                group.append(display->sessionId);
        }
        if (group.size() > 1)
            result.mirrorGroups.append(group);
    }
    return result;
}

} // namespace cutefish::display
