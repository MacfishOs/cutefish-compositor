// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/gestures/wm_gesture_handler.cc
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65

#include "wm/gestures/wm_gesture_handler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "wm/desks/desks_controller.h"
#include "wm/overview/overview_controller.h"

namespace cutefish::wm {

namespace {

double milliseconds(input::Timestamp timestamp)
{
    return std::chrono::duration<double, std::milli>(timestamp.time_since_epoch()).count();
}

} // namespace

WmGestureHandler::WmGestureHandler(DesksController *desks, std::function<bool()> dragging,
                                   OverviewController *overview)
    : m_desks(desks)
    , m_dragging(std::move(dragging))
    , m_overview(overview)
{
}

bool WmGestureHandler::claimGesture(input::Timestamp, input::GestureType type, uint32_t fingers)
{
    if (type != input::GestureType::Swipe || fingers != kDeskSwitchFingers)
        return false;
    m_scroll = {};
    // Still claimed, so the client under the pointer does not get it either.
    m_scroll.ignored = m_dragging && m_dragging();
    if (!m_scroll.ignored && m_warmUp)
        m_warmUp();
    return true;
}

void WmGestureHandler::onGestureUpdate(input::Timestamp timestamp,
                                       const input::GestureUpdate &update)
{
    if (m_scroll.ignored)
        return;
    const QPointF delta = update.unacceleratedDelta * kMmPerUnacceleratedUnit;
    m_scroll.scrollX += delta.x();
    m_scroll.scrollY += delta.y();

    // Update the continuous animation if it has already been started,
    // otherwise start it once it passes the threshold.
    if (m_scroll.horizontalContinuousGestureStarted) {
        m_desks->updateSwipeAnimation(catchUp(delta.x()) / kSwipeLengthPerDeskMm,
                                      milliseconds(timestamp));
        return;
    }
    // Fingers moving up bring the overview in.
    if (m_scroll.verticalContinuousGestureStarted) {
        m_overview->updateSwipe(-catchUp(delta.y()) / kOverviewSwipeLengthMm,
                                milliseconds(timestamp));
        return;
    }
    const double x = std::abs(m_scroll.scrollX);
    const double y = std::abs(m_scroll.scrollY);
    if (std::max(x, y) <= kContinuousGestureMoveThresholdMm)
        return;
    if (m_dragging && m_dragging()) {
        m_scroll.ignored = true;
        return;
    }
    if (y > x) {
        if (!m_overview || !m_overview->startSwipe(/*upward=*/m_scroll.scrollY < 0)) {
            m_scroll.ignored = true;
            return;
        }
        m_scroll.verticalContinuousGestureStarted = true;
        m_scroll.owed = m_scroll.scrollY - delta.y();
        m_overview->updateSwipe(-catchUp(delta.y()) / kOverviewSwipeLengthMm,
                                milliseconds(timestamp));
        return;
    }
    // The overview on its way in or out lands where it was heading, and the
    // desks switch from there.
    if (m_overview && m_overview->isTransitioning())
        m_overview->completeTransition();
    // Fingers moving right bring the desk on the left in.
    if (!m_desks->startSwipeAnimation(/*moveLeft=*/m_scroll.scrollX > 0)) {
        // On the lock screen, with a single desk, or while an animation from
        // another source runs.
        m_scroll.ignored = true;
        return;
    }
    m_scroll.horizontalContinuousGestureStarted = true;
    m_scroll.owed = m_scroll.scrollX - delta.x();
    m_desks->updateSwipeAnimation(catchUp(delta.x()) / kSwipeLengthPerDeskMm,
                                  milliseconds(timestamp));
}

double WmGestureHandler::catchUp(double delta)
{
    // The distance that went into the threshold moves the content too, so it
    // ends up under the fingers: at first as fast again as they move, easing
    // off to their speed. Fingers moving back make up nothing.
    const double owed = m_scroll.owed;
    if (owed == 0 || delta * owed <= 0)
        return delta;
    const double length = kCatchUpTravelRatio * std::abs(owed);
    const auto madeUp = [&](double travel) {
        const double left = 1 - std::min(travel, length) / length;
        return owed * (1 - left * left);
    };
    const double before = madeUp(m_scroll.owedTravel);
    m_scroll.owedTravel += std::abs(delta);
    return delta + madeUp(m_scroll.owedTravel) - before;
}

void WmGestureHandler::onGestureEnd(input::Timestamp timestamp, bool)
{
    const bool horizontal = m_scroll.horizontalContinuousGestureStarted;
    const bool vertical = m_scroll.verticalContinuousGestureStarted;
    m_scroll = {};
    if (horizontal)
        m_desks->endSwipeAnimation(milliseconds(timestamp));
    if (vertical)
        m_overview->endSwipe(milliseconds(timestamp));
}

} // namespace cutefish::wm
