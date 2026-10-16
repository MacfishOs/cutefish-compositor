// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_LAUNCHER_LAUNCHER_PAGER_H_
#define CUTEFISH_SHELL_LAUNCHER_LAUNCHER_PAGER_H_

#include <functional>
#include <memory>
#include <vector>

#include "ui/event.h"

namespace cutefish::ui {
class Animation;
class Window;
} // namespace cutefish::ui

namespace cutefish::shell {

// Two-finger swipes sideways turn the launcher's pages, on the numbers of
// cutefish-framework's paged Scroller (FishUI.WheelHandler { paged: true }):
// the pages follow the fingers, past either end at most a quarter page with
// growing resistance; on release a finger speed over 400 px/s turns one page
// that way, otherwise the nearest page wins; the settle takes
// 180 + 220 × min(1, distance / page) ms on a cubic from the release speed,
// no shorter than 120 ms. Fingers moving up or down page nothing.
class LauncherPager
{
public:
    struct Target {
        // The page width, and the last page's position.
        double viewport = 0;
        double maximum = 0;
    };

    LauncherPager();
    ~LauncherPager();

    LauncherPager(const LauncherPager &) = delete;
    LauncherPager &operator=(const LauncherPager &) = delete;

    std::function<Target()> target;
    std::function<double()> position;
    std::function<void(double)> setPosition;
    // From the first sideways movement until the pages rest.
    std::function<void(bool)> movingChanged;

    // A touchpad scroll event; true when it belongs to a sideways swipe.
    bool handleScroll(ui::Window *window, const ui::ScrollEvent &event);
    // Lands at once where it was heading.
    void stop();
    bool isMoving() const { return m_moving; }

    // The page a release at `position` with `velocity` (px/s, positive
    // towards later pages) settles on, as a position.
    static double pageTarget(double position, double velocity, const Target &target);
    // The position shown for an unbounded finger position.
    static double elastic(double position, const Target &target);

private:
    void setMoving(bool moving);
    // `time` is the stop event's, on the samples' clock.
    void release(ui::Window *window, double time);
    double velocity() const;

    enum class Axis {
        Undecided,
        Horizontal,
        Vertical,
    };

    Axis m_axis = Axis::Undecided;
    bool m_gesture = false;
    bool m_moving = false;
    double m_finger = 0;
    std::vector<std::pair<double, double>> m_samples;
    std::unique_ptr<ui::Animation> m_snap;
    double m_snapTarget = 0;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_LAUNCHER_LAUNCHER_PAGER_H_
