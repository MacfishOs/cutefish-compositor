// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cursor/preferences.h"
#include "cursor/cursor.h"

#include <QDBusConnection>
#include <QDir>
#include <QFileInfo>

namespace cutefish::cursor {
Preferences::Preferences(Cursor &cursor)
    : m_cursor(cursor)
    , m_settings(QSettings::UserScope, QStringLiteral("cutefish"), QStringLiteral("theme"))
{
    const auto directory = QFileInfo(m_settings.fileName()).absolutePath();
    QDir().mkpath(directory);
    m_watcher.addPath(directory);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &Preferences::reload);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &Preferences::reload);
    auto bus = QDBusConnection::sessionBus();
    for (const auto *signal : {"cursorThemeChanged", "cursorSizeChanged"})
        bus.connect(QStringLiteral("com.cutefish.Services"),
                    QStringLiteral("/com/cutefish/Services/Appearance"),
                    QStringLiteral("com.cutefish.Services.Appearance"), QLatin1String(signal), this,
                    SLOT(reload()));
    reload();
}

void Preferences::reload()
{
    m_settings.sync();
    if (!m_watcher.files().contains(m_settings.fileName())
        && QFileInfo::exists(m_settings.fileName()))
        m_watcher.addPath(m_settings.fileName());
    const auto theme =
            m_settings
                    .value(QStringLiteral("CursorTheme"),
                           qEnvironmentVariable("XCURSOR_THEME", QStringLiteral("default")))
                    .toString();
    m_cursor.setTheme(theme, m_settings.value(QStringLiteral("CursorSize"), defaultSize()).toInt());
}
} // namespace cutefish::cursor
