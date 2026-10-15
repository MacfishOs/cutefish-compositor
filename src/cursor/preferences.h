// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <vector>

#include "base/dbus.h"
#include "base/file_watcher.h"
#include "base/settings.h"

namespace cutefish::cursor {
class Cursor;

class Preferences
{
public:
    explicit Preferences(Cursor &cursor);

    void reload();

private:
    Cursor &m_cursor;
    base::Settings m_settings;
    base::FileWatch m_watch;
    std::vector<base::dbus::Match> m_appearanceSignals;
};
} // namespace cutefish::cursor
