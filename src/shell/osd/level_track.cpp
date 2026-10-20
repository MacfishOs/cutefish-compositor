// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/osd/level_track.h"

#include <algorithm>
#include <cmath>

#include "shell/controlcenter/panel_style.h"
#include "ui/animation.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "toolkit/compositor_colors.h"

namespace cutefish::shell {

LevelTrack::LevelTrack() = default;

LevelTrack::~LevelTrack() = default;

void LevelTrack::setPosition(double position)
{
    position = std::clamp(position, 0.0, 1.0);
    if (position == m_target)
        return;
    m_target = position;
    if (!m_animated || !window()) {
        if (m_animation)
            m_animation->stop();
        m_drawn = position;
        requestPaint();
        return;
    }
    if (!m_animation) {
        m_animation = std::make_unique<ui::Animation>(window());
        m_animation->setDuration(std::chrono::milliseconds(120));
        m_animation->setEasing(ui::Animation::Easing::OutCubic);
        m_animation->onUpdate = [this](double value) {
            m_drawn = m_from + (m_target - m_from) * value;
            requestPaint();
        };
    }
    m_animation->stop();
    m_from = m_drawn;
    m_animation->start();
}

void LevelTrack::setIcon(std::string file)
{
    if (file == m_icon)
        return;
    m_icon = std::move(file);
    requestPaint();
}

void LevelTrack::paint(ui::Painter &painter)
{
    const bool dark = ui::Theme::current().darkMode;
    const ui::RectF rect = localRect();
    const double height = rect.height;

    if (height >= 28.0) {
        const double radius = height / 2.0;
        const ui::Color trackBg = dark ? panel::rgba(0.20, 0.22, 0.28, 0.85) : panel::rgba(0, 0, 0, 0.12);

        painter.save();
        painter.clipRoundedRect(rect, radius, ui::Corners::Continuous);

        // Background pill track
        painter.fillRect(rect, trackBg);

        // Filled track
        const double fillWidth = rect.width * m_drawn;
        if (fillWidth > 0) {
            ui::RectF fillRect{0, 0, fillWidth, height};
            const ui::Color trackFill = dark ? panel::rgba(1.0, 1.0, 1.0, 0.95) : ui::Color::rgb(0xFFFFFF);
            painter.fillRect(fillRect, trackFill);
        }
        painter.restore();

        if (!m_icon.empty()) {
            painter.save();
            const double iconSize = 18.0;
            const double iconX = 12.0;
            const double iconY = (height - iconSize) / 2.0;
            const ui::RectF iconRect{iconX, iconY, iconSize, iconSize};
            const bool covered = (fillWidth > iconX + iconSize);
            const ui::Color iconTint = covered ? (dark ? ui::Color::rgb(0x1C1C1E) : ui::Color::rgb(0x383838))
                                               : (dark ? ui::Color::rgb(0xFFFFFF) : ui::Color::rgb(0x383838));
            painter.drawSvg(m_icon, iconRect, iconTint);
            painter.restore();
        }
        return;
    }

    const double travel = rect.width - height;
    const double hairline = 1 / painter.scale();
    const ui::Color outline = ui::color(toolkit::CompositorColor::LevelTrackOutline);
    const ui::RoundedShape pill {height / 2, 0, ui::Corners::Circular};

    painter.drawRoundedRect(rect, pill, ui::color(toolkit::CompositorColor::LevelTrackBackground), hairline,
                            outline);

    const ui::Color fill = ui::color(toolkit::CompositorColor::LevelTrackFill);
    const ui::RectF fillRect {0, 0, height + travel * m_drawn, height};
    painter.drawRoundedRect(fillRect, pill, fill, hairline, outline);

    const ui::RectF knob {fillRect.width - height, 0, height, height};
    const double sigma = (3 * painter.scale() + 1) / 3.3333 / painter.scale();
    painter.drawShadow(knob, height / 2, sigma, ui::color(toolkit::CompositorColor::LevelTrackKnobShadow),
                       {0, 1});
    painter.drawRoundedRect(knob, pill,
                            ui::color(toolkit::CompositorColor::LevelTrackKnob), hairline,
                            outline);

    if (!m_icon.empty()) {
        painter.save();
        painter.multiplyOpacity(dark ? 0.85 : 0.7);
        const ui::RectF icon {(height - kIconSize) / 2, (height - kIconSize) / 2, kIconSize,
                              kIconSize};
        painter.drawSvg(m_icon, icon,
                        dark ? std::optional<ui::Color>(ui::Color::rgb(0xFFFFFF)) : std::nullopt);
        painter.restore();
    }
}

} // namespace cutefish::shell
