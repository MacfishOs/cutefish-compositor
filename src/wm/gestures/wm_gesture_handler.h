// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/gestures/wm_gesture_handler.h
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: the desk half. Upstream switches desks with four
// fingers and keeps three for its overview; here three fingers sideways switch
// desks, and three fingers up and down are claimed but left for an overview.
// libinput swipes stand in for ui::ScrollEvent. The desk content follows the
// fingers whatever the scrolling direction is set to, and by the distance they
// travel on the pad rather than by accelerated pointer motion, which would
// run ahead of a quick swipe and lag a slow one.

#ifndef CUTEFISH_WM_GESTURES_WM_GESTURE_HANDLER_H_
#define CUTEFISH_WM_GESTURES_WM_GESTURE_HANDLER_H_

#include "input/gesture_filter.h"

namespace cutefish::wm {

class DesksController;

class WmGestureHandler : public input::GestureFilter
{
public:
    static constexpr uint32_t kDeskSwitchFingers = 3;

    // How far, in millimetres, the fingers move before a continuous animation
    // starts, to leave out accidental swipes.
    static constexpr double kContinuousGestureMoveThresholdMm = 2;

    // Finger travel that moves the desk content by one desk.
    static constexpr double kSwipeLengthPerDeskMm = 50;

    // libinput's unaccelerated touchpad motion is a thousandth of an inch
    // scaled by the adaptive filter's constant 0.9 * 0.2968 (filter-touchpad.c,
    // touchpad_constant_filter); touchpads keep that profile here.
    static constexpr double kMmPerUnacceleratedUnit = 25.4 / 1000 / (0.9 * 0.2968);

    explicit WmGestureHandler(DesksController *desks);

    bool claimGesture(input::Timestamp timestamp, input::GestureType type,
                      uint32_t fingers) override;
    void onGestureUpdate(input::Timestamp timestamp, const input::GestureUpdate &update) override;
    void onGestureEnd(input::Timestamp timestamp, bool cancelled) override;

private:
    struct ScrollData {
        // In millimetres.
        double scrollX = 0;
        double scrollY = 0;
        bool horizontalContinuousGestureStarted = false;
        // Went up or down, or a desk switch could not start: the rest of the
        // gesture does nothing.
        bool ignored = false;
    };

    DesksController *m_desks = nullptr;
    ScrollData m_scroll;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_GESTURES_WM_GESTURE_HANDLER_H_
