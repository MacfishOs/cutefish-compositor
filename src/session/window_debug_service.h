// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// com.cutefish.WindowDebug1: the window list and direct window placement, for
// checking window-level behaviour from a script instead of by hand. Modelled on
// ash/debug.cc and chromeos/ui/wm/debug_util.cc (PrintWindowHierarchy), which
// dump the same facts for ChromeOS.

#pragma once

#include <QDBusConnection>
#include <QObject>

namespace cutefish::wm {
class WindowManager;
}

namespace cutefish::session {

class WindowDebugService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.WindowDebug1")
public:
    explicit WindowDebugService(wm::WindowManager *manager,
                                QDBusConnection bus = QDBusConnection::sessionBus());
    ~WindowDebugService() override;

    bool registerService();

public slots:
    // Every window bottom to top, as a JSON array. Rects are [x, y, width,
    // height] in logical output coordinates.
    Q_SCRIPTABLE QString ListWindows() const;

    // Each goes through the window's state machine, as a user's move would.
    // False when no window has that id.
    Q_SCRIPTABLE bool MoveWindow(qulonglong id, int x, int y);
    Q_SCRIPTABLE bool SetWindowBounds(qulonglong id, int x, int y, int width, int height);
    Q_SCRIPTABLE bool ActivateWindow(qulonglong id);

private:
    wm::WindowManager *m_manager;
    QDBusConnection m_bus;
    bool m_registered = false;
};

} // namespace cutefish::session
