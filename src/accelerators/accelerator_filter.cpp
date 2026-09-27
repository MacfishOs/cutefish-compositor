// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/wm/core/accelerator_filter.cc and
//           chromium/src/ash/accelerators/pre_target_accelerator_handler.cc
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "accelerators/accelerator_filter.h"

#include <cstdlib>
#include <string>

#include <xkbcommon/xkbcommon.h>

#include "accelerators/accelerator_controller.h"
#include "core/logging.h"
#include "input/seat.h"
#include "input/xkb_tracker.h"
#include "wm/activation.h"
#include "wm/window.h"
#include "wm/window_cycle_controller.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

namespace cutefish::accelerators {

bool usesPositionalShortcuts(const input::XkbTracker &xkb)
{
    std::string layout = xkb.layoutName();
    if (layout.empty()) {
        // No layout was ever set, so xkbcommon compiled its defaults.
        const char *fromEnvironment = std::getenv("XKB_DEFAULT_LAYOUT");
        layout = fromEnvironment ? fromEnvironment : "us";
    }
    return layout.rfind("us", 0) != 0;
}

AcceleratorFilter::AcceleratorFilter(AcceleratorController *controller,
                                     const input::XkbTracker *xkb,
                                     wm::WindowManager *windowManager,
                                     wm::WindowCycleController *windowCycle,
                                     input::EventSink *next, QObject *parent)
    : QObject(parent)
    , m_controller(controller)
    , m_xkb(xkb)
    , m_windowManager(windowManager)
    , m_windowCycle(windowCycle)
    , m_next(next)
{
}

AcceleratorFilter::~AcceleratorFilter() = default;

Accelerator AcceleratorFilter::acceleratorForKey(uint32_t evdevCode, bool pressed) const
{
    uint32_t modifiers = NoModifier;
    if (m_xkb->modifierPressed(XKB_MOD_NAME_SHIFT))
        modifiers |= ShiftDown;
    if (m_xkb->modifierPressed(XKB_MOD_NAME_CTRL))
        modifiers |= ControlDown;
    if (m_xkb->modifierPressed(XKB_MOD_NAME_ALT))
        modifiers |= AltDown;
    if (m_xkb->modifierPressed(XKB_MOD_NAME_LOGO))
        modifiers |= MetaDown;
    if (m_xkb->modifierPressed("Mod5"))
        modifiers |= AltGrDown;
    return Accelerator(m_xkb->baseKeysym(evdevCode), evdevCode, modifiers,
                       pressed ? Accelerator::KeyState::Pressed
                               : Accelerator::KeyState::Released);
}

bool AcceleratorFilter::onPointerMotion(input::Timestamp timestamp, const QPointF &location)
{
    if (m_windowCycle)
        m_windowCycle->onPointerMotion(location);
    return m_next && m_next->onPointerMotion(timestamp, location);
}

bool AcceleratorFilter::onPointerButton(input::Timestamp timestamp, const QPointF &location,
                                        uint32_t evdevButton, bool pressed)
{
    m_controller->launcherStateMachine()->onPointerButton(pressed);
    if (pressed && m_windowCycle && m_windowCycle->onPointerButtonPressed(location))
        return true;
    return m_next && m_next->onPointerButton(timestamp, location, evdevButton, pressed);
}

bool AcceleratorFilter::onPointerAxis(input::Timestamp timestamp, const QPointF &location,
                                      const QPointF &offset, bool discrete)
{
    return m_next && m_next->onPointerAxis(timestamp, location, offset, discrete);
}

bool AcceleratorFilter::onPointerAxisStop(input::Timestamp timestamp, const QPointF &location)
{
    return m_next && m_next->onPointerAxisStop(timestamp, location);
}

bool AcceleratorFilter::onTouchDown(input::Timestamp timestamp, int32_t id,
                                    const QPointF &location)
{
    return m_next && m_next->onTouchDown(timestamp, id, location);
}

bool AcceleratorFilter::onTouchMotion(input::Timestamp timestamp, int32_t id,
                                      const QPointF &location)
{
    return m_next && m_next->onTouchMotion(timestamp, id, location);
}

bool AcceleratorFilter::onTouchUp(input::Timestamp timestamp, int32_t id)
{
    return m_next && m_next->onTouchUp(timestamp, id);
}

void AcceleratorFilter::onTouchCancel(input::Timestamp timestamp)
{
    if (m_next)
        m_next->onTouchCancel(timestamp);
}

bool AcceleratorFilter::onKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed)
{
    const Accelerator accelerator = acceleratorForKey(evdevCode, pressed);

    // Ash registers these on aura::Env, ahead of every other pre-target
    // handler, so they see the key whatever happens to it afterwards.
    m_controller->history()->storeCurrentAccelerator(accelerator);
    m_controller->launcherStateMachine()->onKey(accelerator.keysym(), pressed);

    // The way out of a window holding the pointer or the shortcuts; checked
    // before an inhibitor could hand the key to it.
    input::Seat *seat = m_windowManager ? m_windowManager->seat() : nullptr;
    if (pressed && accelerator.isMetaDown() && accelerator.keysym() == XKB_KEY_Escape && seat
        && seat->breakInputCapture()) {
        return true;
    }

    // While cycling, every key that is not the cycle trigger belongs to the
    // cycle: letting go of the modifiers that started it (Alt, or Command)
    // completes it, Escape cancels it. The client saw those modifiers go down,
    // so it gets their release as well.
    if (m_windowCycle && m_windowCycle->isCycling()
        && !m_controller->doesAcceleratorMatchAction(accelerator,
                                                     AcceleratorAction::CycleForwardMru)
        && !m_controller->doesAcceleratorMatchAction(accelerator,
                                                     AcceleratorAction::CycleBackwardMru)) {
        if (!pressed && m_cycleModifiers && !(accelerator.modifiers() & m_cycleModifiers))
            m_windowCycle->completeCycling();
        else if (m_windowCycle->onKey(accelerator.keysym(), pressed))
            return true;
    }

    if (processAccelerator(accelerator)) {
        if (m_windowCycle && m_windowCycle->isCycling())
            m_cycleModifiers = accelerator.modifiers() & ~ShiftDown;
        return true;
    }
    return m_next && m_next->onKey(timestamp, evdevCode, pressed);
}

bool AcceleratorFilter::processAccelerator(const Accelerator &accelerator)
{
    // Ash also consumes the system keys (volume, brightness, media) here
    // whatever they trigger; they stay with the clients until Cutefish has
    // actions for them.
    if (!shouldProcessAcceleratorNow(accelerator))
        return false;
    return m_controller->process(accelerator);
}

bool AcceleratorFilter::shouldProcessAcceleratorNow(const Accelerator &accelerator) const
{
    // Reserved accelerators (such as the lock key) always have priority.
    if (m_controller->isReserved(accelerator))
        return true;

    // Exo's inhibitor only lets the window consume system keys. A remote
    // desktop or a virtual machine needs Meta as well, so it gets every key
    // but the reserved ones and Meta+Escape.
    input::Seat *seat = m_windowManager ? m_windowManager->seat() : nullptr;
    if (seat && seat->shortcutsInhibition()->isActive())
        return false;

    // Meta+<key> is never the application's otherwise, fullscreen or not, and
    // neither are the preferred ones.
    if (accelerator.isMetaDown() || accelerator.keysym() == XKB_KEY_Super_L
        || accelerator.keysym() == XKB_KEY_Super_R || m_controller->isPreferred(accelerator)) {
        return true;
    }

    // A full screen window has a right to handle all key events including
    // the preferred ones. Ash would offer the rest to the window first and
    // process them afterwards if it declined; without a key-ack extension
    // Cutefish has no afterwards, so everything else is processed now.
    wm::Window *active = m_windowManager ? m_windowManager->activation()->activeWindow() : nullptr;
    const bool fullscreen = active && active->state()->isFullscreen();
    return !fullscreen;
}

} // namespace cutefish::accelerators
