// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "scene/motion.h"

using namespace cutefish::scene::motion;

namespace {

// Central difference of the position, in units per second.
double numericVelocity(const Spring &spring, double time)
{
    constexpr double h = 0.01;
    return (spring.value(time + h) - spring.value(time - h)) / (2 * h / 1000);
}

} // namespace

TEST(MotionTest, CriticallyDampedSpringArrivesWithoutOvershoot)
{
    Spring spring({0.4, 1});
    spring.reset(1000, 0, 100);
    double previous = spring.value(1000);
    EXPECT_DOUBLE_EQ(previous, 0);
    for (double t = 1000; t <= 2000; t += 1) {
        const double value = spring.value(t);
        EXPECT_GE(value, previous - 1e-9);
        EXPECT_LE(value, 100 + 1e-9);
        previous = value;
    }
    // The response is about the time to cover 99% of the way from rest.
    EXPECT_GT(spring.value(1000 + 400), 98.5);
    EXPECT_LT(spring.value(1000 + 400), 99.5);
    EXPECT_NEAR(spring.value(10000), 100, 1e-9);
}

TEST(MotionTest, StartsFromRestAndStaysStillBeforeItsStart)
{
    Spring spring({0.3, 1});
    spring.reset(500, 10, 50);
    EXPECT_DOUBLE_EQ(spring.value(400), 10);
    EXPECT_DOUBLE_EQ(spring.velocity(500), 0);
    EXPECT_GT(spring.value(516), 10);
}

TEST(MotionTest, ClosedFormVelocityMatchesThePath)
{
    for (const double zeta : {0.3, 0.7, 1.0, 1.4, 3.0}) {
        Spring spring({0.35, zeta});
        spring.reset(0, -40, 60, 900);
        for (double t = 5; t < 800; t += 37)
            EXPECT_NEAR(spring.velocity(t), numericVelocity(spring, t), 1e-3) << zeta << " " << t;
    }
}

TEST(MotionTest, SamplingDoesNotChangeTheTrajectory)
{
    Spring spring({0.35, 0.8});
    spring.reset(0, 0, 1);
    const double once = spring.value(333.33);
    for (double t = 0; t < 333.33; t += 13.333)
        spring.value(t);
    EXPECT_DOUBLE_EQ(spring.value(333.33), once);
}

TEST(MotionTest, RetargetKeepsPositionAndVelocity)
{
    Spring spring({0.4, 1});
    spring.reset(0, 0, 800);
    const double value = spring.value(120);
    const double velocity = spring.velocity(120);
    ASSERT_GT(velocity, 0);
    spring.retarget(120, -100);
    EXPECT_NEAR(spring.value(120), value, 1e-9);
    EXPECT_NEAR(spring.velocity(120), velocity, 1e-9);
    // It keeps going the old way for a moment before turning around.
    EXPECT_GT(spring.value(130), value);
    EXPECT_NEAR(spring.value(5000), -100, 1e-6);
}

TEST(MotionTest, UnderdampedSpringOvershootsByTheDampingRatio)
{
    const double zeta = 0.5;
    Spring spring({0.4, zeta});
    spring.reset(0, 0, 1);
    double peak = 0;
    for (double t = 0; t < 2000; t += 0.25)
        peak = std::max(peak, spring.value(t));
    const double expected = std::exp(-std::numbers::pi * zeta / std::sqrt(1 - zeta * zeta));
    EXPECT_NEAR(peak - 1, expected, 1e-4);
}

TEST(MotionTest, ZeroResponseJumpsToTheTarget)
{
    Spring spring({0, 1});
    spring.reset(0, 3, 9, 100);
    EXPECT_DOUBLE_EQ(spring.value(0), 9);
    EXPECT_DOUBLE_EQ(spring.velocity(0), 0);
}
