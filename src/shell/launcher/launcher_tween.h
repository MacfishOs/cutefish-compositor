// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_LAUNCHER_LAUNCHER_TWEEN_H_
#define CUTEFISH_SHELL_LAUNCHER_LAUNCHER_TWEEN_H_

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>

#include "ui/animation.h"

namespace cutefish::shell {

// The easing curves the launcher's animations and transitions use.
namespace easing {

inline double linear(double t)
{
    return t;
}

inline double outCubic(double t)
{
    const double u = t - 1;
    return u * u * u + 1;
}

inline double outQuint(double t)
{
    const double u = t - 1;
    return u * u * u * u * u + 1;
}

// Out back with an overshoot of 1.70158.
inline double outBack(double t)
{
    constexpr double s = 1.70158;
    const double u = t - 1;
    return u * u * ((s + 1) * u + s) + 1;
}

inline double inOutQuad(double t)
{
    return t < 0.5 ? 2 * t * t : -2 * t * t + 4 * t - 1;
}

} // namespace easing

// A value moving from one number to another on the window's frames, along
// one of the curves above.
class LauncherTween
{
public:
    using Curve = double (*)(double);

    LauncherTween() = default;
    LauncherTween(const LauncherTween &) = delete;
    LauncherTween &operator=(const LauncherTween &) = delete;

    // From `from` to `to`; `update` hears each value, the last one `to`.
    void start(ui::Window *window, double from, double to, std::chrono::milliseconds duration, Curve curve,
               std::function<void(double)> update)
    {
        m_to = to;
        m_update = std::move(update);
        if (!window || duration.count() <= 0) {
            finish();
            return;
        }
        if (!m_animation || m_animation->window() != window)
            m_animation = std::make_unique<ui::Animation>(window);
        m_animation->stop();
        m_animation->setEasing(ui::Animation::Easing::Linear);
        m_animation->setDuration(duration);
        m_running = true;
        m_animation->onUpdate = [this, from, to, curve](double t) {
            m_value = from + (to - from) * curve(t);
            if (m_update)
                m_update(m_value);
        };
        m_animation->onFinished = [this] {
            m_running = false;
            m_value = m_to;
        };
        m_value = from;
        m_animation->start();
    }

    // Jumps to the end.
    void finish()
    {
        if (m_animation)
            m_animation->stop();
        m_running = false;
        m_value = m_to;
        if (m_update)
            m_update(m_to);
    }

    // Leaves the value where it is.
    void stop()
    {
        if (m_animation)
            m_animation->stop();
        m_running = false;
    }

    bool isRunning() const { return m_running; }
    double value() const { return m_value; }
    double target() const { return m_to; }

private:
    std::unique_ptr<ui::Animation> m_animation;
    std::function<void(double)> m_update;
    double m_value = 0;
    double m_to = 0;
    bool m_running = false;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_LAUNCHER_LAUNCHER_TWEEN_H_
