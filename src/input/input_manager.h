// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/display/extended_mouse_warp_controller.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: retain the compositor's input dispatch while adding
// projection onto a global logical display topology with holes.

#ifndef CUTEFISH_INPUT_INPUT_MANAGER_H_
#define CUTEFISH_INPUT_INPUT_MANAGER_H_

#include <map>
#include <optional>
#include <set>

#include <QList>
#include <QObject>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QSize>

#include "input/gesture_filter.h"
#include "input/pointer_gesture_delegate.h"
#include "input/seat_observer.h"
#include "input/types.h"

namespace cutefish::display {
class DisplayManager;
}

namespace cutefish::scene {
class Node;
}

namespace cutefish::input {

class EventSink;
struct RelativeMotion;
class Seat;

// Turns the physical event stream into seat events. Physical input dispatch and
// pointer coordinates only: window policy - which window a click activates,
// where a drag moves it - belongs to the EventSink, which in practice is the
// window manager.
//
// Exo has no equivalent: pointer position belongs to Ash's cursor manager and
// activation to Ash's activation client.
class InputManager : public QObject, public SeatObserver
{
    Q_OBJECT

public:
    explicit InputManager(Seat *seat, QObject *parent = nullptr);
    ~InputManager() override;

    Seat *seat() const { return m_seat; }

    // Bounds the pointer may occupy, in output coordinates. Empty means
    // unbounded, which is what the tests use.
    void setDisplayManager(display::DisplayManager *manager) { m_displayManager = manager; }

    void setBounds(const QSize &bounds);
    // `primary` is where the pointer starts: the first topology with displays
    // puts it at the primary display's centre rather than at the origin, as
    // upstream's DrmCursor::OnWindowAdded places a cursor not yet placed.
    void setTopology(const QList<QRect> &displays, const QRect &primary = QRect());

    const QList<QRect> &topology() const { return m_topology; }

    QSize bounds() const { return m_bounds; }

    void setDeviceScale(double scale) { m_deviceScale = scale; }

    void setDeviceBounds(const QSize &bounds) { m_deviceBounds = bounds; }

    QSize deviceBounds() const { return m_deviceBounds.isEmpty() ? m_bounds : m_deviceBounds; }

    // Offered every pointer and key event before the seat sees it. A consumed
    // event is not dispatched to any client. Null in tests that exercise the
    // seat alone.
    void setEventSink(EventSink *sink) { m_eventSink = sink; }

    EventSink *eventSink() const { return m_eventSink; }

    // Takes touchpad gestures the window manager handles itself.
    void setGestureFilter(GestureFilter *filter) { m_gestureFilter = filter; }

    QPointF pointerPosition() const { return m_pointerPosition; }

    // Places the pointer without generating an event. Used at startup to put it
    // in the middle of the output.
    void warpPointer(const QPointF &position);

    // Re-targets the still pointer once control returns to the event loop, as
    // aura's WindowEventDispatcher::PostSynthesizeMouseMove: a surface mapped,
    // hidden or restacked under it gets its enter or leave without a move.
    void synthesizeMotion();

    // The compositor's own cursor, shown wherever no client owns the pointer
    // image. A placeholder until the compositor installs its cursor theme.
    // `hotspot` is its tip, relative to the node's own origin.
    void setFallbackCursor(scene::Node *node, const QPoint &hotspot);

    // --- Called by the libinput backend ---

    // `delta` is in output pixels; `unaccelerated` is the device's delta before
    // acceleration, which only relative-pointer clients see.
    void onPointerMotion(Timestamp timestamp, const QPointF &delta, const QPointF &unaccelerated);

    void onPointerMotion(Timestamp timestamp, const QPointF &delta)
    {
        onPointerMotion(timestamp, delta, delta);
    }
    void onPointerMotionAbsolute(Timestamp timestamp, const QPointF &position);
    // Normalized absolute coordinates map to the current logical desktop extent.
    void onPointerMotionNormalized(Timestamp timestamp, const QPointF &position);
    void onPointerButton(Timestamp timestamp, uint32_t evdevButton, bool pressed);
    void onPointerAxis(Timestamp timestamp, const QPointF &offset, bool discrete);
    void onPointerAxisStop(Timestamp timestamp);
    // Touchpad gestures. `delta` is in output pixels, like motion.
    void onPointerGestureBegin(Timestamp timestamp, GestureType type, uint32_t fingers);
    void onPointerGestureUpdate(Timestamp timestamp, GestureType type, const QPointF &delta,
                                const QPointF &unacceleratedDelta, double scale, double rotation);
    void onPointerGestureEnd(Timestamp timestamp, GestureType type, bool cancelled);
    // `layout` says where the keyboard has its Command key, if any. Under the
    // Command convention a PC keyboard's Alt and Meta trade places, so that
    // the key next to the space bar is Command (Meta) on every keyboard. The
    // Pc convention leaves every keyboard as it is.
    void onKey(Timestamp timestamp, uint32_t evdevCode, bool pressed,
               KeyboardModifierLayout layout = KeyboardModifierLayout::Pc);

    // Touchscreen input. `normalized` is the point on the panel, 0..1 across
    // it in its own orientation; the display it belongs to places it on the
    // desktop. A frame closes each batch of points from the device.
    void onTouchDown(Timestamp timestamp, int32_t id, const QPointF &normalized);
    void onTouchMotion(Timestamp timestamp, int32_t id, const QPointF &normalized);
    void onTouchUp(Timestamp timestamp, int32_t id);
    void onTouchCancel(Timestamp timestamp);
    void onTouchFrame(Timestamp timestamp);

Q_SIGNALS:
    void userActivity();

private:
    void onSurfaceFocused(compositor::Surface *, compositor::Surface *) override { }

    void onCursorChanged() override { updateFallbackCursor(); }

    QPointF clamp(const QPointF &position) const;

    uint32_t positionalKey(uint32_t evdevCode, KeyboardModifierLayout layout) const;

    void dispatchMotion(Timestamp timestamp, const RelativeMotion *relative = nullptr);

    // Moves the pointer towards `target` as far as the active pointer
    // constraint allows.
    void movePointer(Timestamp timestamp, QPointF target, const RelativeMotion &relative);
    void moveAbsolute(Timestamp timestamp, const QPointF &target);

    void updateFallbackCursor();
    std::optional<QPointF> touchLocation(const QPointF &normalized) const;

    display::DisplayManager *m_displayManager = nullptr;
    Seat *m_seat = nullptr;
    EventSink *m_eventSink = nullptr;
    GestureFilter *m_gestureFilter = nullptr;
    // The gesture in progress went to the filter.
    bool m_gestureClaimed = false;
    QSet<uint32_t> m_secureKeys;
    // Physical key to the code it went down as, so that its release matches
    // whatever convention is in force by then.
    std::map<uint32_t, uint32_t> m_positionalKeys;
    bool m_motionConsumed = false;
    bool m_synthesizePending = false;
    QSize m_bounds;
    QList<QRect> m_topology;
    QSize m_deviceBounds;
    double m_deviceScale = 1;
    QPointF m_pointerPosition;
    bool m_pointerPlaced = false;
    // The last layout with displays, which the pointer position refers to: a
    // reprobe republishes the layout through an empty one.
    QList<QRect> m_pointerLayout;
    // Where the last absolute event put the device, which a lock keeps apart
    // from the pointer.
    std::optional<QPointF> m_absolutePosition;
    // Where each touch point is, for the release that carries no location.
    std::map<int32_t, QPointF> m_touchLocations;
    // Points the event sink took, whose events it keeps getting.
    std::set<int32_t> m_consumedTouches;
    // The point a locked session sees as its pointer: the lock screen takes
    // input the way the mouse gives it, one point at a time.
    std::optional<int32_t> m_secureTouch;
    scene::Node *m_fallbackCursor = nullptr;
    QPoint m_fallbackCursorHotspot;
};

} // namespace cutefish::input

#endif // CUTEFISH_INPUT_INPUT_MANAGER_H_
