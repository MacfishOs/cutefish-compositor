// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Development-only WM controls, installed only by explicit demo options.

#ifndef CUTEFISH_WM_DEMO_CONTROLLER_H_
#define CUTEFISH_WM_DEMO_CONTROLLER_H_

#include <functional>
#include <vector>

#include "base/geometry.h"
#include "base/timer.h"
#include "input/event_sink.h"

namespace cutefish::compositor {
class Surface;
}

namespace cutefish::input {
class InputManager;
}

namespace cutefish::wm {

class Window;
class WindowManager;

class DemoController : public input::EventSink
{
public:
    DemoController(WindowManager* manager, input::InputManager* input);
    ~DemoController() override;

    // Creates a Window for every mapped, roleless root surface that does not
    // have one yet. Wired to the compositor's repaint signal.
    void adoptNewSurfaces();

    // Runs the whole window-manager API as a timed sequence instead of waiting
    // for keys, so the hardware run can be driven over SSH. Each step logs what
    // it did; the harness checks the log.
    void startScript(int stepIntervalMs = 700);

    // input::EventSink: shortcuts first, then the window manager's own policy.
    bool onPointerMotion(input::Timestamp timestamp, const base::PointF &location) override;
    bool onPointerButton(input::Timestamp timestamp, const base::PointF &location,
                         uint32_t evdevButton, bool pressed) override;
    bool onKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed) override;
    bool onTouchDown(input::Timestamp timestamp, int32_t id, const base::PointF &location) override;
    bool onTouchMotion(input::Timestamp timestamp, int32_t id, const base::PointF &location) override;
    bool onTouchUp(input::Timestamp timestamp, int32_t id) override;
    void onTouchCancel(input::Timestamp timestamp) override;

private:
    bool handleShortcut(uint32_t evdevCode);
    Window* target() const;
    void activateNext();
    void restoreAllMinimized();
    void toggleTransientOnActive();

    void buildScript();
    void runNextScriptStep();
    // The window the script drives. Re-validated each step, because a client
    // may exit underneath it.
    Window* scriptTarget();

    WindowManager* m_manager = nullptr;
    input::InputManager* m_input = nullptr;

    Window* m_scriptTarget = nullptr;
    base::PointF m_dragAnchor;
    std::vector<std::function<void()>> m_script;
    size_t m_scriptStep = 0;
    int m_scriptIntervalMs = 0;
    base::Timer m_scriptTimer;
    bool m_scriptRunning = false;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_DEMO_CONTROLLER_H_
