// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/mouse_settings.h"

#include <QDBusError>
#include <QDebug>
#include <cmath>

namespace cutefish::input {
namespace {
constexpr auto service = "com.cutefish.Mouse1";
constexpr auto path = "/com/cutefish/Mouse1";

QString preferenceFile()
{
    QSettings settings(QSettings::UserScope, QStringLiteral("cutefish"), QStringLiteral("mouse"));
    return settings.fileName();
}
} // namespace

MouseSettings::MouseSettings(QObject *parent, const QString &settingsFile)
    : QObject(parent)
    , m_settings(settingsFile.isEmpty() ? preferenceFile() : settingsFile, QSettings::IniFormat)
{
    m_leftHanded = m_settings.value(QStringLiteral("leftHanded"), false).toBool();
    m_naturalScroll = m_settings.value(QStringLiteral("naturalScroll"), false).toBool();
    // The legacy accelerationProfile preference means flat (acceleration disabled).
    m_acceleration = !m_settings.value(QStringLiteral("accelerationProfile"), false).toBool();
    bool valid = false;
    const double speed =
            m_settings.value(QStringLiteral("pointerAcceleration"), 0).toDouble(&valid);
    if (valid && std::isfinite(speed))
        m_pointerAcceleration = qBound(-1.0, speed, 1.0);
}

MouseSettings::~MouseSettings()
{
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QLatin1String(path));
        bus.unregisterService(QLatin1String(service));
    }
}

bool MouseSettings::registerService()
{
    if (m_registered)
        return true;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(path), this,
                            QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots
                                    | QDBusConnection::ExportAllSignals)) {
        qWarning() << "Cannot export mouse settings:" << bus.lastError().message();
        return false;
    }
    if (!bus.registerService(QLatin1String(service))) {
        qWarning() << "Cannot register mouse settings:" << bus.lastError().message();
        bus.unregisterObject(QLatin1String(path));
        return false;
    }
    m_registered = true;
    return true;
}

void MouseSettings::save(const QString &key, const QVariant &value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        qWarning() << "Cannot persist mouse settings to" << m_settings.fileName();
    emit settingsChanged();
}

void MouseSettings::setAvailable(bool available)
{
    if (m_available == available)
        return;
    m_available = available;
    emit availableChanged();
}

void MouseSettings::setLeftHanded(bool enabled)
{
    if (m_leftHanded == enabled)
        return;
    m_leftHanded = enabled;
    save(QStringLiteral("leftHanded"), enabled);
    emit leftHandedChanged();
}

void MouseSettings::setNaturalScroll(bool enabled)
{
    if (m_naturalScroll == enabled)
        return;
    m_naturalScroll = enabled;
    save(QStringLiteral("naturalScroll"), enabled);
    emit naturalScrollChanged();
}

void MouseSettings::setAcceleration(bool enabled)
{
    if (m_acceleration == enabled)
        return;
    m_acceleration = enabled;
    save(QStringLiteral("accelerationProfile"), !enabled);
    emit accelerationChanged();
}

void MouseSettings::setPointerAcceleration(double value)
{
    if (!std::isfinite(value))
        return;
    value = qBound(-1.0, value, 1.0);
    if (m_pointerAcceleration == value)
        return;
    m_pointerAcceleration = value;
    save(QStringLiteral("pointerAcceleration"), value);
    emit pointerAccelerationChanged();
}
} // namespace cutefish::input
