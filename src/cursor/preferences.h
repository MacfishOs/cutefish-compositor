// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QSettings>

namespace cutefish::cursor {
class Cursor;

class Preferences : public QObject
{
    Q_OBJECT
public:
    explicit Preferences(Cursor &cursor);

public slots:
    void reload();

private:
    Cursor &m_cursor;
    QSettings m_settings;
    QFileSystemWatcher m_watcher;
};
} // namespace cutefish::cursor
