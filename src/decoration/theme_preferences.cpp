// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "decoration/theme_preferences.h"
#include "core/preferences.h"
#include "decoration/decoration_theme.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDir>
#include <QFileInfo>

namespace cutefish::decoration {
ThemePreferences::ThemePreferences(Theme &theme)
    : m_theme(theme)
    , m_settings(preferencesFile(QStringLiteral("services")), QSettings::IniFormat)
{
    m_settings.beginGroup(QStringLiteral("Appearance"));
    const auto directory = QFileInfo(m_settings.fileName()).absolutePath();
    QDir().mkpath(directory);
    m_watcher.addPath(directory);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &ThemePreferences::reload);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &ThemePreferences::reload);
    QDBusConnection::sessionBus().connect(
            QStringLiteral("com.cutefish.Services"),
            QStringLiteral("/com/cutefish/Services/Appearance"),
            QStringLiteral("com.cutefish.Services.Appearance"), QStringLiteral("darkModeChanged"),
            this, SLOT(setDarkMode(bool)));
    QDBusConnection::sessionBus().connect(
            QStringLiteral("com.cutefish.Services"),
            QStringLiteral("/com/cutefish/Services/Appearance"),
            QStringLiteral("com.cutefish.Services.Appearance"),
            QStringLiteral("continuousCornersChanged"), this, SLOT(setContinuousCorners(bool)));
    QDBusConnection::sessionBus().connect(
            QStringLiteral("com.cutefish.Services"),
            QStringLiteral("/com/cutefish/Services/Appearance"),
            QStringLiteral("com.cutefish.Services.Appearance"),
            QStringLiteral("accentColorChanged"), this, SLOT(setAccentColor(int)));
    QDBusConnection::sessionBus().connect(
            QStringLiteral("com.cutefish.Services"),
            QStringLiteral("/com/cutefish/Services/Appearance"),
            QStringLiteral("com.cutefish.Services.Appearance"),
            QStringLiteral("systemFontPointSizeChanged"), this, SLOT(reloadCaptionPointSize()));
    reload();
}

void ThemePreferences::reload()
{
    m_settings.sync();
    if (!m_watcher.files().contains(m_settings.fileName())
        && QFileInfo::exists(m_settings.fileName()))
        m_watcher.addPath(m_settings.fileName());
    setDarkMode(m_settings.value(QStringLiteral("DarkMode"), false).toBool());
    setContinuousCorners(m_settings.value(QStringLiteral("ContinuousCorners"), true).toBool());
    setAccentColor(m_settings.value(QStringLiteral("AccentColor"), 0).toInt());
    setCaptionPointSize(m_settings.value(QStringLiteral("FontSize"), 9).toReal());
}

void ThemePreferences::reloadCaptionPointSize()
{
    QDBusInterface appearance(QStringLiteral("com.cutefish.Services"),
                              QStringLiteral("/com/cutefish/Services/Appearance"),
                              QStringLiteral("com.cutefish.Services.Appearance"));
    const QVariant points = appearance.property("systemFontPointSize");
    if (points.isValid())
        setCaptionPointSize(points.toReal());
    else
        reload();
}

void ThemePreferences::setCaptionPointSize(qreal points)
{
    if (points > 0)
        m_theme.setCaptionPointSize(points);
}

void ThemePreferences::setContinuousCorners(bool continuous)
{
    m_theme.setContinuousCorners(continuous);
}

void ThemePreferences::setAccentColor(int index)
{
    m_theme.setAccentColor(index);
}

void ThemePreferences::setDarkMode(bool dark)
{
    m_theme.setDarkMode(dark);
}
} // namespace cutefish::decoration
