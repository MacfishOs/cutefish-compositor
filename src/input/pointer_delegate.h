// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/pointer_delegate.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.
//
// Adaptation: gfx::PointF/Vector2dF become QPointF, and the pinch/stylus and
// relative-motion delegates are out of scope for v0.1.

#ifndef CUTEFISH_INPUT_POINTER_DELEGATE_H_
#define CUTEFISH_INPUT_POINTER_DELEGATE_H_

#include <QPointF>

#include "input/types.h"

namespace cutefish::compositor {
class Surface;
}

namespace cutefish::input {

class Pointer;

// Handles events on pointers in context-specific ways.
class PointerDelegate
{
public:
    // Called at the top of the pointer's destructor, to give the delegate a
    // chance to remove itself.
    virtual void onPointerDestroying(Pointer* pointer) = 0;

    // Whether `surface` is a valid target for this pointer, e.g. because it is
    // owned by the same client.
    virtual bool canAcceptPointerEventsForSurface(compositor::Surface* surface) const = 0;

    // `location` is relative to the origin of `surface`; `pressedButtonFlags`
    // holds every button currently down.
    virtual void onPointerEnter(compositor::Surface* surface, const QPointF& location,
                                ButtonFlags pressedButtonFlags) = 0;
    virtual void onPointerLeave(compositor::Surface* surface) = 0;
    virtual void onPointerMotion(Timestamp timestamp, const QPointF& location) = 0;

    // `changedButtonFlags` holds every button that changed state.
    virtual void onPointerButton(Timestamp timestamp, ButtonFlags changedButtonFlags,
                                 bool pressed) = 0;

    // `offset` is the direction and distance of the scroll in Wayland axis
    // units, where one wheel click is 10. `discrete` is true for a device with
    // detents, such as a wheel.
    virtual void onPointerScroll(Timestamp timestamp, const QPointF& offset,
                                 bool discrete) = 0;

    // Ends a sequence of finger scroll events, e.g. fingers lifted off a
    // touchpad.
    virtual void onFingerScrollStop(Timestamp timestamp) = 0;

    // Called once every pointer event of this frame has been sent. Nothing is
    // delivered to the client before this.
    virtual void onPointerFrame() = 0;

protected:
    virtual ~PointerDelegate() = default;
};

} // namespace cutefish::input

#endif // CUTEFISH_INPUT_POINTER_DELEGATE_H_
