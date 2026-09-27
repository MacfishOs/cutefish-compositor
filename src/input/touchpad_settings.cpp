// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/touchpad_settings.h"

#include <QDBusError>
#include <QDebug>
#include <cmath>

namespace cutefish::input {
namespace {
constexpr auto service = "com.cutefish.Touchpad1";
constexpr auto path = "/com/cutefish/Touchpad1";

QString preferenceFile()
{
    QSettings settings(QSettings::UserScope, QStringLiteral("cutefish"),
                       QStringLiteral("touchpad"));
    return settings.fileName();
}
} // namespace

TouchpadSettings::TouchpadSettings(QObject *parent, const QString &settingsFile)
    : QObject(parent)
    , m_settings(settingsFile.isEmpty() ? preferenceFile() : settingsFile, QSettings::IniFormat)
{
    const auto flag = [this](const char *key, bool fallback) {
        return m_settings.value(QLatin1String(key), fallback).toBool();
    };
    m_enabled = flag("enabled", m_enabled);
    m_disableWithMouse = flag("disableWithMouse", m_disableWithMouse);
    m_disableWhileTyping = flag("disableWhileTyping", m_disableWhileTyping);
    m_tapToClick = flag("tapToClick", m_tapToClick);
    m_tapDragging = flag("tapDragging", m_tapDragging);
    m_naturalScroll = flag("naturalScroll", m_naturalScroll);
    m_twoFingerSecondaryClick = flag("twoFingerSecondaryClick", m_twoFingerSecondaryClick);
    bool valid = false;
    const double speed =
            m_settings.value(QStringLiteral("pointerAcceleration"), 0).toDouble(&valid);
    if (valid && std::isfinite(speed))
        m_pointerAcceleration = qBound(-1.0, speed, 1.0);
    const double scroll =
            m_settings.value(QStringLiteral("scrollSpeed"), kDefaultScrollSpeed).toDouble(&valid);
    if (valid && std::isfinite(scroll))
        m_scrollSpeed = qBound(kMinimumScrollSpeed, scroll, kMaximumScrollSpeed);
}

TouchpadSettings::~TouchpadSettings()
{
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QLatin1String(path));
        bus.unregisterService(QLatin1String(service));
    }
}

bool TouchpadSettings::registerService()
{
    if (m_registered)
        return true;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(path), this,
                            QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots
                                    | QDBusConnection::ExportAllSignals)) {
        qWarning() << "Cannot export touchpad settings:" << bus.lastError().message();
        return false;
    }
    if (!bus.registerService(QLatin1String(service))) {
        qWarning() << "Cannot register touchpad settings:" << bus.lastError().message();
        bus.unregisterObject(QLatin1String(path));
        return false;
    }
    m_registered = true;
    return true;
}

void TouchpadSettings::save(const QString &key, const QVariant &value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        qWarning() << "Cannot persist touchpad settings to" << m_settings.fileName();
    emit settingsChanged();
}

void TouchpadSettings::setAvailable(bool available)
{
    if (m_available == available)
        return;
    m_available = available;
    emit availableChanged();
}

void TouchpadSettings::setSecondaryClickConfigurable(bool configurable)
{
    if (m_secondaryClickConfigurable == configurable)
        return;
    m_secondaryClickConfigurable = configurable;
    emit secondaryClickConfigurableChanged();
}

void TouchpadSettings::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    save(QStringLiteral("enabled"), enabled);
    emit enabledChanged();
}

void TouchpadSettings::setDisableWithMouse(bool enabled)
{
    if (m_disableWithMouse == enabled)
        return;
    m_disableWithMouse = enabled;
    save(QStringLiteral("disableWithMouse"), enabled);
    emit disableWithMouseChanged();
}

void TouchpadSettings::setDisableWhileTyping(bool enabled)
{
    if (m_disableWhileTyping == enabled)
        return;
    m_disableWhileTyping = enabled;
    save(QStringLiteral("disableWhileTyping"), enabled);
    emit disableWhileTypingChanged();
}

void TouchpadSettings::setTapToClick(bool enabled)
{
    if (m_tapToClick == enabled)
        return;
    m_tapToClick = enabled;
    save(QStringLiteral("tapToClick"), enabled);
    emit tapToClickChanged();
}

void TouchpadSettings::setTapDragging(bool enabled)
{
    if (m_tapDragging == enabled)
        return;
    m_tapDragging = enabled;
    save(QStringLiteral("tapDragging"), enabled);
    emit tapDraggingChanged();
}

void TouchpadSettings::setNaturalScroll(bool enabled)
{
    if (m_naturalScroll == enabled)
        return;
    m_naturalScroll = enabled;
    save(QStringLiteral("naturalScroll"), enabled);
    emit naturalScrollChanged();
}

void TouchpadSettings::setPointerAcceleration(double value)
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

void TouchpadSettings::setScrollSpeed(double value)
{
    if (!std::isfinite(value))
        return;
    value = qBound(kMinimumScrollSpeed, value, kMaximumScrollSpeed);
    if (m_scrollSpeed == value)
        return;
    m_scrollSpeed = value;
    save(QStringLiteral("scrollSpeed"), value);
    emit scrollSpeedChanged();
}

void TouchpadSettings::setTwoFingerSecondaryClick(bool enabled)
{
    if (m_twoFingerSecondaryClick == enabled)
        return;
    m_twoFingerSecondaryClick = enabled;
    save(QStringLiteral("twoFingerSecondaryClick"), enabled);
    emit twoFingerSecondaryClickChanged();
}
} // namespace cutefish::input
