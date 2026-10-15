// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// com.cutefish.WindowDebug1: the window list and direct window placement, for
// checking window-level behaviour from a script instead of by hand. Modelled on
// ash/debug.cc and chromeos/ui/wm/debug_util.cc (PrintWindowHierarchy), which
// dump the same facts for ChromeOS.

#pragma once

#include "base/dbus.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cutefish::input {
class InputManager;
}

namespace cutefish::wm {
class WindowManager;
}

namespace cutefish::session {

class WindowDebugService
{
public:
    // The session bus when no connection is given.
    explicit WindowDebugService(wm::WindowManager *manager, base::dbus::Connection *bus = nullptr);
    WindowDebugService(const WindowDebugService &) = delete;
    WindowDebugService &operator=(const WindowDebugService &) = delete;
    ~WindowDebugService();

    bool registerService();

    // Headless runs only: the input methods below then feed it as a device
    // would. A session on real hardware never sets it.
    void setInput(input::InputManager *input) { m_input = input; }

    // Supplies one scheduling stats line per output, as the compositor logs on
    // seat changes and at exit.
    void setOutputStats(std::function<std::vector<std::string>()> stats)
    {
        m_outputStats = std::move(stats);
    }

    // Every window bottom to top, as a JSON array. Rects are [x, y, width,
    // height] in logical output coordinates.
    std::string ListWindows() const;
    // Each goes through the window's state machine, as a user's move would.
    // False when no window has that id.
    bool MoveWindow(uint64_t id, int x, int y);
    bool SetWindowBounds(uint64_t id, int x, int y, int width, int height);
    bool ActivateWindow(uint64_t id);
    bool MinimizeWindow(uint64_t id);
    // Snaps to the left half when primary, the right half otherwise.
    bool SnapWindow(uint64_t id, bool primary);
    bool RestoreWindow(uint64_t id);
    // The outputs' scheduling counters, one line each, read live. Empty in a
    // headless compositor.
    std::vector<std::string> OutputStats() const;
    // Pointer and keyboard input for driving a headless compositor from a
    // script, in logical output coordinates and evdev codes. False without
    // an input to feed.
    bool PointerMotion(double x, double y);
    bool PointerButton(uint32_t button, bool pressed);
    bool Key(uint32_t code, bool pressed);

private:
    wm::WindowManager *m_manager;
    input::InputManager *m_input = nullptr;
    std::function<std::vector<std::string>()> m_outputStats;
    base::dbus::Connection *m_bus;
    std::unique_ptr<base::dbus::Object> m_object;
    bool m_registered = false;
};

} // namespace cutefish::session
