// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "scene/motion.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace cutefish::scene::motion {

namespace {

// Below this the closed forms for the under- and overdamped cases lose
// precision; the critically damped one is exact there.
constexpr double kCriticalTolerance = 1e-4;

} // namespace

double durationScale()
{
    static const double scale = [] {
        const char *value = std::getenv("CUTEFISH_ANIMATION_DURATION_SCALE");
        if (!value)
            return 1.0;
        char *end = nullptr;
        const double parsed = std::strtod(value, &end);
        return end != value && std::isfinite(parsed) && parsed > 0 ? parsed : 1.0;
    }();
    return scale;
}

double cubicBezier(double x1, double y1, double x2, double y2, double x)
{
    const auto at = [](double a, double b, double s) {
        return 3 * a * (1 - s) * (1 - s) * s + 3 * b * (1 - s) * s * s + s * s * s;
    };
    x = std::clamp(x, 0.0, 1.0);
    double low = 0;
    double high = 1;
    for (int i = 0; i < 32; ++i) {
        const double middle = (low + high) / 2;
        (at(x1, x2, middle) < x ? low : high) = middle;
    }
    return at(y1, y2, (low + high) / 2);
}

Spring::Spring()
{
    updateCoefficients();
}

Spring::Spring(const SpringParameters &parameters)
    : m_parameters(parameters)
{
    updateCoefficients();
}

void Spring::updateCoefficients()
{
    const double response = m_parameters.response * durationScale();
    m_omega = response > 0 ? 2 * std::numbers::pi / response : 0;
    m_zeta = std::max(0.0, m_parameters.dampingRatio);
}

void Spring::reset(double time, double value, double target, double velocity)
{
    m_start = time;
    m_target = target;
    m_displacement = value - target;
    m_velocity = velocity;
}

void Spring::retarget(double time, double target)
{
    reset(time, value(time), target, velocity(time));
}

double Spring::elapsed(double time) const
{
    return std::max(0.0, time - m_start) / 1000;
}

double Spring::displacement(double t) const
{
    const double x0 = m_displacement;
    const double v0 = m_velocity;
    if (m_omega <= 0)
        return 0;
    const double w = m_omega;
    if (std::abs(m_zeta - 1) < kCriticalTolerance)
        return std::exp(-w * t) * (x0 + (v0 + w * x0) * t);
    if (m_zeta < 1) {
        const double decay = m_zeta * w;
        const double wd = w * std::sqrt(1 - m_zeta * m_zeta);
        return std::exp(-decay * t)
                * (x0 * std::cos(wd * t) + (v0 + decay * x0) / wd * std::sin(wd * t));
    }
    const double root = std::sqrt(m_zeta * m_zeta - 1);
    const double r1 = -w * (m_zeta - root);
    const double r2 = -w * (m_zeta + root);
    const double c2 = (v0 - r1 * x0) / (r2 - r1);
    const double c1 = x0 - c2;
    return c1 * std::exp(r1 * t) + c2 * std::exp(r2 * t);
}

double Spring::displacementVelocity(double t) const
{
    const double x0 = m_displacement;
    const double v0 = m_velocity;
    if (m_omega <= 0)
        return 0;
    const double w = m_omega;
    if (std::abs(m_zeta - 1) < kCriticalTolerance)
        return std::exp(-w * t) * (v0 - w * (v0 + w * x0) * t);
    if (m_zeta < 1) {
        const double decay = m_zeta * w;
        const double wd = w * std::sqrt(1 - m_zeta * m_zeta);
        return std::exp(-decay * t)
                * (v0 * std::cos(wd * t) - (w * w * x0 + decay * v0) / wd * std::sin(wd * t));
    }
    const double root = std::sqrt(m_zeta * m_zeta - 1);
    const double r1 = -w * (m_zeta - root);
    const double r2 = -w * (m_zeta + root);
    const double c2 = (v0 - r1 * x0) / (r2 - r1);
    const double c1 = x0 - c2;
    return c1 * r1 * std::exp(r1 * t) + c2 * r2 * std::exp(r2 * t);
}

double Spring::value(double time) const
{
    return m_target + displacement(elapsed(time));
}

double Spring::velocity(double time) const
{
    return displacementVelocity(elapsed(time));
}

} // namespace cutefish::scene::motion
