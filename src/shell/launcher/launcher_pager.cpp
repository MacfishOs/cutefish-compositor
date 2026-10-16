// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/launcher/launcher_pager.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "ui/animation.h"

namespace cutefish::shell {

static constexpr double kPageFlickSpeed = 400;
static constexpr double kSnapDuration = 180;
static constexpr double kSnapPageDuration = 220;
static constexpr double kMinimumSnapDuration = 120;
// The release velocity is a line fitted to the samples of this window, and
// none at all when the fingers rested this long before lifting.
static constexpr double kVelocityWindow = 100;
static constexpr double kVelocityTimeout = 80;

static double nowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

LauncherPager::LauncherPager() = default;
LauncherPager::~LauncherPager() = default;

double LauncherPager::elastic(double position, const Target &target)
{
    const double edge = std::clamp(position, 0.0, std::max(0.0, target.maximum));
    const double distance = position - edge;
    if (target.viewport <= 0)
        return edge;
    // Each pixel past an end gives less, up to a quarter of a page.
    const double limit = target.viewport / 4;
    return edge + limit * distance / (std::abs(distance) + limit);
}

double LauncherPager::pageTarget(double position, double velocity, const Target &target)
{
    if (target.viewport <= 0)
        return position;
    const double page = position / target.viewport;
    double result = std::round(page);
    if (velocity > kPageFlickSpeed)
        result = std::floor(page) + 1;
    else if (velocity < -kPageFlickSpeed)
        result = std::ceil(page) - 1;
    return std::clamp(result * target.viewport, 0.0, std::max(0.0, target.maximum));
}

void LauncherPager::setMoving(bool moving)
{
    if (moving == m_moving)
        return;
    m_moving = moving;
    if (movingChanged)
        movingChanged(moving);
}

bool LauncherPager::handleScroll(ui::Window *window, const ui::ScrollEvent &event)
{
    if (!target || !position || !setPosition)
        return false;
    if (event.stop) {
        const bool ours = m_gesture && m_axis == Axis::Horizontal;
        if (ours)
            release(window, event.time ? double(event.time) : nowMs());
        m_gesture = false;
        m_axis = Axis::Undecided;
        return ours;
    }
    if (!m_gesture) {
        m_gesture = true;
        m_axis = Axis::Undecided;
        m_samples.clear();
    }
    if (m_axis == Axis::Undecided) {
        if (event.deltaX == 0 && event.deltaY == 0)
            return false;
        m_axis = std::abs(event.deltaX) >= std::abs(event.deltaY) ? Axis::Horizontal : Axis::Vertical;
        if (m_axis == Axis::Horizontal) {
            // Fingers catch a settling page where it is shown.
            if (m_snap)
                m_snap->stop();
            const Target bounds = target();
            const double shown = position();
            m_finger = std::clamp(shown, 0.0, std::max(0.0, bounds.maximum));
            if (shown != m_finger && bounds.viewport > 0) {
                // Back from the elastic curve to where fingers would be.
                const double limit = bounds.viewport / 4;
                const double over = std::min(std::abs(shown - m_finger), limit * 0.999);
                m_finger += (shown > m_finger ? 1 : -1) * limit * over / (limit - over);
            }
            setMoving(true);
        }
    }
    if (m_axis != Axis::Horizontal)
        return false;
    m_finger += event.deltaX;
    const double time = event.time ? double(event.time) : nowMs();
    m_samples.push_back({time, m_finger});
    while (m_samples.size() > 2 && time - m_samples.front().first > kVelocityWindow)
        m_samples.erase(m_samples.begin());
    setPosition(elastic(m_finger, target()));
    return true;
}

double LauncherPager::velocity() const
{
    if (m_samples.size() < 2)
        return 0;
    // A least-squares line through the samples, in pixels per second.
    double meanT = 0, meanX = 0;
    for (const auto &[t, x] : m_samples) {
        meanT += t;
        meanX += x;
    }
    meanT /= double(m_samples.size());
    meanX /= double(m_samples.size());
    double covariance = 0, variance = 0;
    for (const auto &[t, x] : m_samples) {
        covariance += (t - meanT) * (x - meanX);
        variance += (t - meanT) * (t - meanT);
    }
    return variance > 0 ? covariance / variance * 1000 : 0;
}

void LauncherPager::release(ui::Window *window, double time)
{
    const Target bounds = target();
    const double from = position();
    double speed = velocity();
    if (!m_samples.empty() && time - m_samples.back().first > kVelocityTimeout)
        speed = 0;
    m_samples.clear();
    m_snapTarget = pageTarget(m_finger, speed, bounds);
    const double distance = std::abs(m_snapTarget - from);
    double duration = kSnapDuration + kSnapPageDuration * std::min(1.0, distance / std::max(1.0, bounds.viewport));
    // OutCubic starts at three times its average speed. Fingers faster than
    // that shorten it, so a quick release does not brake the page.
    if ((m_snapTarget - from) * speed > 0)
        duration = std::max(kMinimumSnapDuration, std::min(duration, 3000 * distance / std::abs(speed)));
    if (distance == 0 || !window) {
        setPosition(m_snapTarget);
        setMoving(false);
        return;
    }
    const double startVelocity = (m_snapTarget - from) * speed > 0 ? speed : 3000 * (m_snapTarget - from) / duration;
    if (!m_snap || m_snap->window() != window)
        m_snap = std::make_unique<ui::Animation>(window);
    m_snap->stop();
    m_snap->setEasing(ui::Animation::Easing::Linear);
    m_snap->setDuration(std::chrono::milliseconds(int64_t(std::lround(duration))));
    const double to = m_snapTarget;
    m_snap->onUpdate = [this, from, to, startVelocity, duration](double progress) {
        // A cubic Hermite from the release speed to rest on the page.
        const double remaining = 1.0 - progress;
        const double value = progress >= 1 ? to
                                           : to + (from - to) * (1.0 + 2.0 * progress) * remaining * remaining
                        + startVelocity * duration / 1000 * progress * remaining * remaining;
        setPosition(value);
    };
    m_snap->onFinished = [this] { setMoving(false); };
    m_snap->start();
}

void LauncherPager::stop()
{
    m_gesture = false;
    m_axis = Axis::Undecided;
    m_samples.clear();
    if (m_snap && m_snap->isRunning()) {
        m_snap->stop();
        setPosition(m_snapTarget);
    }
    setMoving(false);
}

} // namespace cutefish::shell
