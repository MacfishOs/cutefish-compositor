// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/dock/dock_tooltip.h"

#include <algorithm>
#include <cmath>

#include "shell/dock/dock.h"
#include "shell/dock/dock_window.h"
#include "shell/shell_data.h"
#include "shell/shell_host.h"
#include "shell/shell_window.h"
#include "ui/bubble_shape.h"
#include "ui/embedded_window.h"
#include "ui/label.h"
#include "ui/theme.h"
#include "ui/window.h"

namespace cutefish::shell {

// How far past the ends of the dock a tooltip may reach.
static constexpr double kReach = 240;

DockTooltip::DockTooltip(DockWindow *dock, Shell *shell)
    : m_dock(dock)
    , m_shell(shell)
{
}

DockTooltip::~DockTooltip()
{
    m_window.reset();
}

bool DockTooltip::isShown() const
{
    return m_window && !m_window->isHidden();
}

void DockTooltip::hide()
{
    if (m_window)
        m_window->hide();
}

void DockTooltip::show(const std::string &text, const base::PointF &target)
{
    m_text = text;
    if (text.empty()) {
        hide();
        return;
    }
    const DockSettings::Direction direction = m_dock->dock().settings().direction();
    const bool across = direction != DockSettings::Bottom;
    const ui::Theme &theme = ui::Theme::current();
    ui::BubbleShape bubble;
    bubble.radius = theme.mediumRadius;
    bubble.continuousCorners = true;
    bubble.arrowEdge = direction == DockSettings::Left ? ui::BubbleShape::Edge::Left
            : direction == DockSettings::Right        ? ui::BubbleShape::Edge::Right
                                                      : ui::BubbleShape::Edge::Bottom;

    // The label's size decides the bubble's, before any window is made.
    if (!m_measure) {
        m_measure = m_dock->window()->window()->contentView()->addChild<ui::Label>();
        m_measure->setElide(ui::Label::Elide::None);
        m_measure->setVisible(false);
    }
    m_measure->setText(text);
    const ui::SizeF textSize = m_measure->sizeHint();
    const double width = std::ceil(std::max(textSize.width + theme.largeSpacing * 1.5
                                                    + (across ? bubble.arrowLength : 0),
                                            across ? 0 : bubble.minimumEdgeLength()));
    const double height = std::ceil(std::max(textSize.height + theme.largeSpacing
                                                      + (across ? 0 : bubble.arrowLength),
                                              across ? bubble.minimumEdgeLength() : 0));

    // The still window, in the dock window's coordinates.
    const base::Rect dock = m_dock->panelRect();
    const base::Rect window = m_dock->window()->geometry();
    const Shell::Output *output = m_shell->output({});
    base::Rect available = output ? m_shell->workArea(*output) : base::Rect();
    available.translate(-window.x(), -window.y());
    const double gap = theme.smallSpacing;
    const double dockRight = dock.x() + dock.width();
    const double availableRight = available.x() + available.width();
    ui::RectF area;
    if (across) {
        const double top = std::max(double(available.y()), dock.y() - kReach);
        const double h = std::max(height, std::min(double(available.y() + available.height()),
                                                   dock.y() + dock.height() + kReach)
                                                  - top);
        if (direction == DockSettings::Left) {
            const double left = dockRight + gap;
            area = {left, top, std::max(width, std::min(2 * kReach, availableRight - left)), h};
        } else {
            const double w = std::max(width, std::min(2 * kReach, dock.x() - gap - available.x()));
            area = {dock.x() - gap - w, top, w, h};
        }
    } else {
        const double start = std::max(double(available.x()), dock.x() - kReach);
        area = {start, dock.y() - gap - height,
                std::max(width, std::min(availableRight, dockRight + kReach) - start), height};
    }

    double x = 0;
    double y = 0;
    if (direction == DockSettings::Right)
        x = area.width - width;
    else if (!across)
        x = std::round(std::clamp(target.x() - area.x - width / 2, 0.0, area.width - width));
    if (across)
        y = std::round(std::clamp(target.y() - area.y - height / 2, 0.0, area.height - height));
    bubble.width = width;
    bubble.height = height;
    bubble.arrowPosition = across ? target.y() - area.y - y : target.x() - area.x - x;
    const ui::RectF body = bubble.bodyRect();

    if (!m_window || area != m_area) {
        if (m_window)
            m_window->hide();
        m_area = area;
        ui::PopupPlacement placement;
        placement.anchorRect = {area.x, area.y, 1, 1};
        placement.anchor = ui::PopupAnchor::TopLeft;
        placement.gravity = ui::PopupAnchor::BottomRight;
        placement.flipX = placement.flipY = placement.slide = false;
        m_window = ui::Window::createPopup(m_dock->window()->window(), placement, false);
        auto content = std::make_unique<ui::View>();
        m_label = content->addChild<ui::Label>();
        m_label->setElide(ui::Label::Elide::None);
        m_window->setContentView(std::move(content));
        m_window->resize({area.width, area.height});
    }
    m_label->setText(text);
    m_label->setGeometry({x + body.x + (body.width - textSize.width) / 2,
                          y + body.y + (body.height - textSize.height) / 2, textSize.width,
                          textSize.height});
    m_window->show();

    // The glass is the body; the arrow outside it continues the glass, and
    // the outline follows the whole bubble.
    ShellWindow *popup =
            m_shell->host()->windowFor(static_cast<ui::EmbeddedWindow *>(m_window.get()));
    if (!popup)
        return;
    const render::GlassParams glass = theme.darkMode ? popupGlass(0.17, 0.17, 0.19, 0.6)
                                                     : popupGlass(1, 1, 1, 0.6);
    const base::Rect region(int(std::floor(x + body.x)), int(std::floor(y + body.y)),
                            int(std::ceil(body.width)), int(std::ceil(body.height)));
    const double radius = std::min(bubble.radius, std::min(body.width, body.height) / 2);
    popup->setBackgroundBlur(region, int(std::lround(radius)), glass, bubble.path().translated(x, y));
}

} // namespace cutefish::shell
