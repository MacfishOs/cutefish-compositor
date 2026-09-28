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
#include <QStringList>
#include <functional>

namespace cutefish::input {
class InputManager;
}

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

    // Headless runs only: the input methods below then feed it as a device
    // would. A session on real hardware never sets it.
    void setInput(input::InputManager *input) { m_input = input; }

    // Supplies one scheduling stats line per output, as the compositor logs on
    // seat changes and at exit.
    void setOutputStats(std::function<QStringList()> stats) { m_outputStats = std::move(stats); }

public slots:
    // Every window bottom to top, as a JSON array. Rects are [x, y, width,
    // height] in logical output coordinates.
    Q_SCRIPTABLE QString ListWindows() const;

    // Each goes through the window's state machine, as a user's move would.
    // False when no window has that id.
    Q_SCRIPTABLE bool MoveWindow(qulonglong id, int x, int y);
    Q_SCRIPTABLE bool SetWindowBounds(qulonglong id, int x, int y, int width, int height);
    Q_SCRIPTABLE bool ActivateWindow(qulonglong id);

    // The outputs' scheduling counters, one line each, read live. Empty in a
    // headless compositor.
    Q_SCRIPTABLE QStringList OutputStats() const;

    // Pointer and keyboard input for driving a headless compositor from a
    // script, in logical output coordinates and evdev codes. False without
    // an input to feed.
    Q_SCRIPTABLE bool PointerMotion(double x, double y);
    Q_SCRIPTABLE bool PointerButton(uint button, bool pressed);
    Q_SCRIPTABLE bool Key(uint code, bool pressed);

private:
    wm::WindowManager *m_manager;
    input::InputManager *m_input = nullptr;
    std::function<QStringList()> m_outputStats;
    QDBusConnection m_bus;
    bool m_registered = false;
};

} // namespace cutefish::session
