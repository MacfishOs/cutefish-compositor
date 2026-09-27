// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/touchscreen_settings.h"

#include <QDBusError>
#include <QDebug>

namespace cutefish::input {
namespace {
constexpr auto service = "com.cutefish.Touchscreen1";
constexpr auto path = "/com/cutefish/Touchscreen1";

QString preferenceFile()
{
    QSettings settings(QSettings::UserScope, QStringLiteral("cutefish"),
                       QStringLiteral("touchscreen"));
    return settings.fileName();
}
} // namespace

TouchscreenSettings::TouchscreenSettings(QObject *parent, const QString &settingsFile)
    : QObject(parent)
    , m_settings(settingsFile.isEmpty() ? preferenceFile() : settingsFile, QSettings::IniFormat)
{
    m_disabledDevices = m_settings.value(QStringLiteral("disabledDevices")).toStringList();
    m_disabledDevices.removeAll(QString());
    m_disabledDevices.removeDuplicates();
}

TouchscreenSettings::~TouchscreenSettings()
{
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QLatin1String(path));
        bus.unregisterService(QLatin1String(service));
    }
}

bool TouchscreenSettings::registerService()
{
    if (m_registered)
        return true;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(path), this,
                            QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots
                                    | QDBusConnection::ExportAllSignals)) {
        qWarning() << "Cannot export touchscreen settings:" << bus.lastError().message();
        return false;
    }
    if (!bus.registerService(QLatin1String(service))) {
        qWarning() << "Cannot register touchscreen settings:" << bus.lastError().message();
        bus.unregisterObject(QLatin1String(path));
        return false;
    }
    m_registered = true;
    return true;
}

void TouchscreenSettings::setDevices(const QStringList &devices)
{
    if (m_devices == devices)
        return;
    m_devices = devices;
    emit devicesChanged();
}

void TouchscreenSettings::setDeviceEnabled(const QString &name, bool enabled)
{
    if (name.isEmpty() || deviceEnabled(name) == enabled)
        return;
    if (enabled)
        m_disabledDevices.removeAll(name);
    else
        m_disabledDevices.append(name);
    m_settings.setValue(QStringLiteral("disabledDevices"), m_disabledDevices);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        qWarning() << "Cannot persist touchscreen settings to" << m_settings.fileName();
    emit disabledDevicesChanged();
    emit settingsChanged();
}
} // namespace cutefish::input
