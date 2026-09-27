// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WM_DOCK_PREFERENCES_H_
#define CUTEFISH_WM_DOCK_PREFERENCES_H_

#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>

namespace cutefish::wm {

class DockLayoutManager;

// Follows the dock's visibility setting into the dock layout: the shared
// `cutefish/dock` settings file the dock writes when the user changes it.
class DockPreferences : public QObject
{
    Q_OBJECT

public:
    explicit DockPreferences(DockLayoutManager &layout, QObject *parent = nullptr);

public Q_SLOTS:
    void reload();

private:
    DockLayoutManager &m_layout;
    QSettings m_settings;
    QFileSystemWatcher m_watcher;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_DOCK_PREFERENCES_H_
