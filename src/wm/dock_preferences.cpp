// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/dock_preferences.h"

#include <QDir>
#include <QFileInfo>

#include "core/preferences.h"
#include "wm/dock_layout_manager.h"

namespace cutefish::wm {

namespace {

// The dock's DockSettings::Visibility, by value.
DockAutoHideBehavior behaviorFromSetting(int visibility)
{
    switch (visibility) {
    case 1:
        return DockAutoHideBehavior::Always;
    case 2:
        return DockAutoHideBehavior::WhenObstructed;
    default:
        return DockAutoHideBehavior::Never;
    }
}

} // namespace

DockPreferences::DockPreferences(DockLayoutManager &layout, QObject *parent)
    : QObject(parent)
    , m_layout(layout)
    , m_settings(preferencesFile(QStringLiteral("shell")), QSettings::IniFormat)
{
    m_settings.beginGroup(QStringLiteral("Dock"));
    const auto directory = QFileInfo(m_settings.fileName()).absolutePath();
    QDir().mkpath(directory);
    m_watcher.addPath(directory);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &DockPreferences::reload);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &DockPreferences::reload);
    reload();
}

void DockPreferences::reload()
{
    m_settings.sync();
    if (!m_watcher.files().contains(m_settings.fileName())
        && QFileInfo::exists(m_settings.fileName()))
        m_watcher.addPath(m_settings.fileName());
    m_layout.setBehavior(
            behaviorFromSetting(m_settings.value(QStringLiteral("Visibility"), 0).toInt()));
}

} // namespace cutefish::wm
