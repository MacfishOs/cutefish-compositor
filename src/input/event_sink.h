// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The replacement for ui::EventHandler: something that sees physical input
// before the seat dispatches it to clients, and may consume it.

#ifndef CUTEFISH_INPUT_EVENT_SINK_H_
#define CUTEFISH_INPUT_EVENT_SINK_H_

#include <cstdint>

#include <QPointF>

#include "input/types.h"

namespace cutefish::input {

class EventSink
{
public:
    virtual ~EventSink() = default;

    // Each returns true if the sink consumed the event, in which case the seat
    // does not dispatch it to any client. `location` is in output coordinates.
    virtual bool onPointerMotion(Timestamp timestamp, const QPointF& location) { return false; }
    virtual bool onPointerButton(Timestamp timestamp, const QPointF& location,
                                 uint32_t evdevButton, bool pressed)
    {
        return false;
    }
    virtual bool onPointerAxis(Timestamp, const QPointF&, const QPointF&, bool) { return false; }
    virtual bool onPointerAxisStop(Timestamp, const QPointF&) { return false; }
    virtual bool onKey(Timestamp timestamp, uint32_t evdevCode, bool pressed) { return false; }

    // A touch point landing at `location`. Returning true takes the whole
    // point: its motion, release or cancellation come here and none of it
    // reaches a client. A point a client has can still be taken later, by
    // returning true from its motion or release, as a window drag does once
    // the client asked to move with it.
    virtual bool onTouchDown(Timestamp, int32_t, const QPointF&) { return false; }
    virtual bool onTouchMotion(Timestamp, int32_t, const QPointF&) { return false; }
    virtual bool onTouchUp(Timestamp, int32_t) { return false; }
    // Every point is gone, taken or not.
    virtual void onTouchCancel(Timestamp) { }
};

} // namespace cutefish::input

#endif // CUTEFISH_INPUT_EVENT_SINK_H_
