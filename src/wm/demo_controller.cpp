// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/demo_controller.h"

#include <algorithm>
#include <linux/input-event-codes.h>

#include <QTimer>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "core/logging.h"
#include "input/input_manager.h"
#include "scene/node.h"
#include "scene/surface_node.h"
#include "wm/activation.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

namespace cutefish::wm {

DemoController::DemoController(WindowManager* manager, input::InputManager* input,
                               QObject* parent)
    : QObject(parent)
    , m_manager(manager)
    , m_input(input)
{
    qCInfo(cfCompositor,
           "WM demo keys: F1 maximize  F2 restore  F3 minimize  F4 unminimize all  "
           "F5 fullscreen  F6 move  F7 resize  F8 transient  F9 next window");
}

DemoController::~DemoController() = default;

void DemoController::adoptNewSurfaces()
{
    compositor::Compositor* compositor = m_manager->compositor();
    if (!compositor)
        return;

    for (compositor::Surface* surface : compositor->surfaces()) {
        // Subsurfaces belong to their parent's window, and a surface with a
        // role (the pointer cursor) is not a window at all.
        if (surface->parentSurface() || surface->hasRole() || !surface->isMapped())
            continue;
        if (m_manager->windowForSurface(surface))
            continue;

        Window* window = m_manager->createWindow(surface);
        // xdg_toplevel.set_min_size sets this for real clients. Without it the
        // model's 1x1 default lets an interactive resize collapse the window.
        window->setMinimumSize(QSize(120, 90));
        qCInfo(cfCompositor) << "WM demo adopted a surface as a window at" << window->bounds();
        m_manager->activation()->activate(window);
    }
}

Window* DemoController::target() const
{
    return m_manager->activation()->activeWindow();
}

bool DemoController::onPointerMotion(input::Timestamp timestamp, const QPointF& location)
{
    return m_manager->onPointerMotion(timestamp, location);
}

bool DemoController::onPointerButton(input::Timestamp timestamp, const QPointF& location,
                                     uint32_t evdevButton, bool pressed)
{
    return m_manager->onPointerButton(timestamp, location, evdevButton, pressed);
}

bool DemoController::onKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed)
{
    if (pressed && handleShortcut(evdevCode))
        return true;
    return m_manager->onKey(timestamp, evdevCode, pressed);
}

bool DemoController::onTouchDown(input::Timestamp timestamp, int32_t id, const QPointF& location)
{
    return m_manager->onTouchDown(timestamp, id, location);
}

bool DemoController::onTouchMotion(input::Timestamp timestamp, int32_t id,
                                   const QPointF& location)
{
    return m_manager->onTouchMotion(timestamp, id, location);
}

bool DemoController::onTouchUp(input::Timestamp timestamp, int32_t id)
{
    return m_manager->onTouchUp(timestamp, id);
}

void DemoController::onTouchCancel(input::Timestamp timestamp)
{
    m_manager->onTouchCancel(timestamp);
}

bool DemoController::handleShortcut(uint32_t evdevCode)
{
    Window* window = target();

    switch (evdevCode) {
    case KEY_F1:
        if (window)
            window->maximize();
        return true;
    case KEY_F2:
        if (window)
            window->restore();
        return true;
    case KEY_F3:
        if (window)
            window->minimize();
        return true;
    case KEY_F4:
        restoreAllMinimized();
        return true;
    case KEY_F5:
        if (window)
            window->setFullscreen(!window->state()->isFullscreen());
        return true;
    case KEY_F6:
        qCDebug(cfInputTransitions) << "demo F6 begin move request";
        if (window) {
            m_manager->eventHandler()->beginMove(window, m_input->pointerPosition());
        }
        return true;
    case KEY_F7:
        qCDebug(cfInputTransitions) << "demo F7 begin resize request";
        if (window) {
            m_manager->eventHandler()->beginResize(window, ResizeEdge::BottomRight,
                                                   m_input->pointerPosition());
        }
        return true;
    case KEY_F8:
        toggleTransientOnActive();
        return true;
    case KEY_F9:
        activateNext();
        return true;
    default:
        return false;
    }
}

void DemoController::activateNext()
{
    const std::vector<Window*>& windows = m_manager->windows();
    if (windows.empty())
        return;

    Window* active = target();
    const auto it = std::find(windows.begin(), windows.end(), active);
    const size_t start = it == windows.end() ? 0 : (it - windows.begin() + 1);

    for (size_t i = 0; i < windows.size(); ++i) {
        Window* candidate = windows[(start + i) % windows.size()];
        if (candidate != active && m_manager->activation()->canActivate(candidate)) {
            m_manager->activation()->activate(candidate);
            return;
        }
    }
}

void DemoController::restoreAllMinimized()
{
    for (Window* window : std::vector<Window*>(m_manager->windows())) {
        if (window->state()->isMinimized())
            window->restore();
    }
}

void DemoController::toggleTransientOnActive()
{
    Window* window = target();
    const std::vector<Window*>& windows = m_manager->windows();
    if (!window || windows.size() < 2)
        return;

    if (window->transientParent()) {
        window->setTransientParent(nullptr);
        qCInfo(cfCompositor) << "WM demo cleared the transient parent";
        return;
    }
    for (Window* candidate : windows) {
        if (candidate != window && window->setTransientParent(candidate)) {
            qCInfo(cfCompositor) << "WM demo made the active window transient for the one below";
            return;
        }
    }
}

void DemoController::startScript(int stepIntervalMs)
{
    m_scriptIntervalMs = stepIntervalMs;
    buildScript();
    qCInfo(cfCompositor, "WM demo script: %zu steps, %d ms apart", m_script.size(),
           stepIntervalMs);
    // Wait for a client to map something before the first step.
    QTimer::singleShot(m_scriptIntervalMs, this, [this] {
        runNextScriptStep();
    });
}

void DemoController::runNextScriptStep()
{
    if (m_scriptStep >= m_script.size()) {
        qCInfo(cfCompositor, "WM demo script: finished");
        return;
    }

    // Nothing to drive until a client has mapped a window; keep waiting rather
    // than running the script against nothing.
    if (m_manager->windows().empty()) {
        QTimer::singleShot(m_scriptIntervalMs, this, [this] {
            runNextScriptStep();
        });
        return;
    }

    m_script[m_scriptStep++]();
    QTimer::singleShot(m_scriptIntervalMs, this, [this] {
        runNextScriptStep();
    });
}

Window* DemoController::scriptTarget()
{
    const std::vector<Window*>& windows = m_manager->windows();
    if (std::find(windows.begin(), windows.end(), m_scriptTarget) == windows.end())
        m_scriptTarget = windows.empty() ? nullptr : windows.front();
    return m_scriptTarget;
}

void DemoController::buildScript()
{
    m_script.clear();
    m_scriptStep = 0;

    auto log = [this](const char* what) {
        Window* window = scriptTarget();
        qCInfo(cfCompositor, "WM demo script: %s -> windows=%zu state=%s bounds=%d,%d %dx%d", what,
               m_manager->windows().size(), window ? toString(window->stateType()) : "none",
               window ? window->bounds().x() : 0, window ? window->bounds().y() : 0,
               window ? window->bounds().width() : 0, window ? window->bounds().height() : 0);
    };

    m_script.push_back([this, log] {
        if (Window* window = scriptTarget())
            m_manager->activation()->activate(window);
        log("activate");
    });

    // Interactive move, one leg per step so it is visible on screen.
    //
    // Every drag is driven by warping the pointer and feeding motion through
    // InputManager, exactly as libinput would. Calling the handler directly
    // would leave the physical pointer somewhere else, and the next real motion
    // event would yank the window over to it.
    m_script.push_back([this, log] {
        Window* window = scriptTarget();
        if (!window)
            return;
        m_dragAnchor = QPointF(window->bounds().topLeft());
        m_input->warpPointer(m_dragAnchor);
        m_manager->eventHandler()->beginMove(window, m_dragAnchor);
        log("begin-move");
    });
    for (int i = 1; i <= 3; ++i) {
        m_script.push_back([this, log, i] {
            m_input->onPointerMotionAbsolute(input::Timestamp::clock::now(),
                                             m_dragAnchor + QPointF(60.0 * i, 40.0 * i));
            log("move");
        });
    }
    m_script.push_back([this, log] {
        m_manager->eventHandler()->onPointerButtonReleased();
        log("end-move");
    });

    // Interactive resize from the bottom-right corner.
    m_script.push_back([this, log] {
        Window* window = scriptTarget();
        if (!window)
            return;
        m_dragAnchor = QPointF(window->bounds().bottomRight());
        m_input->warpPointer(m_dragAnchor);
        m_manager->eventHandler()->beginResize(window, ResizeEdge::BottomRight, m_dragAnchor);
        log("begin-resize");
    });
    for (int i = 1; i <= 3; ++i) {
        m_script.push_back([this, log, i] {
            m_input->onPointerMotionAbsolute(input::Timestamp::clock::now(),
                                             m_dragAnchor + QPointF(40.0 * i, 30.0 * i));
            log("resize");
        });
    }
    m_script.push_back([this, log] {
        m_manager->eventHandler()->onPointerButtonReleased();
        log("end-resize");
    });

    m_script.push_back([this, log] {
        if (Window* window = scriptTarget())
            window->maximize();
        log("maximize");
    });
    m_script.push_back([this, log] {
        if (Window* window = scriptTarget())
            window->restore();
        log("restore-from-maximized");
    });
    m_script.push_back([this, log] {
        if (Window* window = scriptTarget())
            window->setFullscreen(true);
        log("fullscreen");
    });
    m_script.push_back([this, log] {
        if (Window* window = scriptTarget())
            window->setFullscreen(false);
        log("restore-from-fullscreen");
    });
    m_script.push_back([this, log] {
        if (Window* window = scriptTarget())
            window->minimize();
        log("minimize");
    });
    m_script.push_back([this, log] {
        restoreAllMinimized();
        if (Window* window = scriptTarget())
            m_manager->activation()->activate(window);
        log("unminimize");
    });
    m_script.push_back([this, log] {
        activateNext();
        qCInfo(cfCompositor, "WM demo script: active window is now %p",
               static_cast<void*>(m_manager->activation()->activeWindow()));
        log("activate-next");
    });
    m_script.push_back([this, log] {
        if (Window* window = scriptTarget()) {
            for (Window* candidate : m_manager->windows()) {
                if (candidate != window && window->setTransientParent(candidate))
                    break;
            }
            qCInfo(cfCompositor, "WM demo script: transient parent is %p",
                   static_cast<void*>(window->transientParent()));
        }
        log("set-transient");
    });
    m_script.push_back([this, log] {
        if (Window* window = scriptTarget())
            window->setTransientParent(nullptr);
        log("clear-transient");
    });
}

} // namespace cutefish::wm
