// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/keyboard_settings.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

#include "core/preferences.h"
#include "input/types.h"

namespace cutefish::input {
namespace {
constexpr auto service = "com.cutefish.Keyboard1";
constexpr auto path = "/com/cutefish/Keyboard1";

constexpr auto pcConvention = "pc";
constexpr auto commandConvention = "command";

// What xkbcommon compiles for empty rule names.
QString defaultLayout()
{
    QString layout = qEnvironmentVariable("XKB_DEFAULT_LAYOUT", QStringLiteral("us"));
    const QString variant = qEnvironmentVariable("XKB_DEFAULT_VARIANT");
    if (!variant.isEmpty())
        layout += QLatin1Char('-') + variant;
    return layout;
}

int storedMilliseconds(const QSettings &settings, const QString &key, int fallback, int min,
                       int max)
{
    bool valid = false;
    const int value = settings.value(key, fallback).toInt(&valid);
    return valid ? qBound(min, value, max) : fallback;
}
} // namespace

KeyboardSettings::KeyboardSettings(QObject *parent, const QString &settingsFile)
    : QObject(parent)
    , m_settings(settingsFile.isEmpty() ? preferencesFile(QStringLiteral("compositor"))
                                       : settingsFile,
                 QSettings::IniFormat)
{
    m_settings.beginGroup(QStringLiteral("Keyboard"));
    m_repeatEnabled = m_settings.value(QStringLiteral("repeatEnabled"), true).toBool();
    m_repeatDelay = storedMilliseconds(m_settings, QStringLiteral("repeatDelay"),
                                       kDefaultKeyRepeatDelayMs, kMinRepeatDelayMs,
                                       kMaxRepeatDelayMs);
    m_repeatInterval = storedMilliseconds(m_settings, QStringLiteral("repeatInterval"),
                                          kDefaultKeyRepeatIntervalMs, kMinRepeatIntervalMs,
                                          kMaxRepeatIntervalMs);
    m_layout = m_settings.value(QStringLiteral("layout")).toString();
    if (m_settings.value(QStringLiteral("shortcutConvention")).toString()
        == QLatin1String(commandConvention))
        m_shortcutConvention = ShortcutConvention::Command;
}

KeyboardSettings::~KeyboardSettings()
{
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QLatin1String(path));
        bus.unregisterService(QLatin1String(service));
    }
}

bool KeyboardSettings::registerService()
{
    if (m_registered)
        return true;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(path), this,
                            QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots
                                    | QDBusConnection::ExportAllSignals)) {
        qWarning() << "Cannot export keyboard settings:" << bus.lastError().message();
        return false;
    }
    if (!bus.registerService(QLatin1String(service))) {
        qWarning() << "Cannot register keyboard settings:" << bus.lastError().message();
        bus.unregisterObject(QLatin1String(path));
        return false;
    }
    m_registered = true;
    return true;
}

void KeyboardSettings::setLayoutApplier(LayoutApplier applier)
{
    m_applier = std::move(applier);
    if (m_layout.isEmpty() || !m_applier || m_applier(m_layout))
        return;
    qWarning() << "Cannot use the saved keyboard layout" << m_layout;
    m_layout.clear();
    emit layoutChanged();
}

QString KeyboardSettings::layout() const
{
    return m_layout.isEmpty() ? defaultLayout() : m_layout;
}

QString KeyboardSettings::shortcutConventionName() const
{
    return QLatin1String(m_shortcutConvention == ShortcutConvention::Command ? commandConvention
                                                                             : pcConvention);
}

// An invalid value removes the key.
void KeyboardSettings::save(const QString &key, const QVariant &value)
{
    if (value.isValid())
        m_settings.setValue(key, value);
    else
        m_settings.remove(key);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        qWarning() << "Cannot persist keyboard settings to" << m_settings.fileName();
}

void KeyboardSettings::setRepeatEnabled(bool enabled)
{
    if (m_repeatEnabled == enabled)
        return;
    m_repeatEnabled = enabled;
    save(QStringLiteral("repeatEnabled"), enabled);
    emit repeatEnabledChanged();
}

void KeyboardSettings::setRepeatDelay(int milliseconds)
{
    milliseconds = qBound(kMinRepeatDelayMs, milliseconds, kMaxRepeatDelayMs);
    if (m_repeatDelay == milliseconds)
        return;
    m_repeatDelay = milliseconds;
    save(QStringLiteral("repeatDelay"), milliseconds);
    emit repeatDelayChanged();
}

void KeyboardSettings::setRepeatInterval(int milliseconds)
{
    milliseconds = qBound(kMinRepeatIntervalMs, milliseconds, kMaxRepeatIntervalMs);
    if (m_repeatInterval == milliseconds)
        return;
    m_repeatInterval = milliseconds;
    save(QStringLiteral("repeatInterval"), milliseconds);
    emit repeatIntervalChanged();
}

bool KeyboardSettings::setLayout(const QString &name)
{
    if (name == m_layout)
        return true;
    if (!m_applier || !m_applier(name))
        return false;
    m_layout = name;
    save(QStringLiteral("layout"), name.isEmpty() ? QVariant() : QVariant(name));
    emit layoutChanged();
    return true;
}

bool KeyboardSettings::setShortcutConvention(const QString &name)
{
    ShortcutConvention convention;
    if (name == QLatin1String(pcConvention))
        convention = ShortcutConvention::Pc;
    else if (name == QLatin1String(commandConvention))
        convention = ShortcutConvention::Command;
    else
        return false;
    if (convention == m_shortcutConvention)
        return true;
    m_shortcutConvention = convention;
    save(QStringLiteral("shortcutConvention"),
         convention == ShortcutConvention::Pc ? QVariant() : QVariant(name));
    emit shortcutConventionChanged();
    return true;
}

} // namespace cutefish::input
