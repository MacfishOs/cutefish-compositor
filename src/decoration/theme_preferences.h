// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>

namespace cutefish::decoration {
class Theme;

// Follows the session's dark mode, corner style, accent and font size into the
// server-side frame and window silhouettes: the shared `cutefish/theme`
// settings file, and the Appearance service's signals for the changes
// themselves, since the service writes the file lazily.
class ThemePreferences : public QObject
{
    Q_OBJECT
public:
    explicit ThemePreferences(Theme &theme);

public slots:
    void reload();
    void setDarkMode(bool dark);
    void setContinuousCorners(bool continuous);
    void setAccentColor(int index);
    void setCaptionPointSize(qreal points);
    // The font size signal carries no value, so it is read back from the service.
    void reloadCaptionPointSize();

private:
    Theme &m_theme;
    QSettings m_settings;
    QFileSystemWatcher m_watcher;
};
} // namespace cutefish::decoration
