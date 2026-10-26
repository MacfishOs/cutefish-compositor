// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/tiling/window_tile_popover.h"

#include <algorithm>
#include <cmath>

#include "core/main_loop.h"
#include "shell/shell_window.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

namespace cutefish::shell {

enum class TileAction {
    None,
    LeftHalf,
    RightHalf,
    TopHalf,
    BottomHalf,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Zoom,
    FullScreen,
};

class WindowTilePopover::PopoverView final : public ui::View
{
public:
    explicit PopoverView(WindowTilePopover *popover)
        : m_popover(popover)
    {
        setTracksHover(true);

        m_header1 = addChild<ui::Label>();
        m_header1->setText("Move & Resize");
        m_header1->setFontSize(10.5);
        m_header1->setWeight(600);
        m_header1->setGeometry({14, 10, 208, 16});

        m_header2 = addChild<ui::Label>();
        m_header2->setText("Quarters");
        m_header2->setFontSize(10.5);
        m_header2->setWeight(600);
        m_header2->setGeometry({14, 76, 208, 16});

        m_zoomLabel = addChild<ui::Label>();
        m_zoomLabel->setText("Fill Screen");
        m_zoomLabel->setFontSize(12.0);
        m_zoomLabel->setWeight(500);
        m_zoomLabel->setGeometry({42, 148, 184, 20});

        m_fullScreenLabel = addChild<ui::Label>();
        m_fullScreenLabel->setText("Enter Full Screen");
        m_fullScreenLabel->setFontSize(12.0);
        m_fullScreenLabel->setWeight(500);
        m_fullScreenLabel->setGeometry({42, 180, 184, 20});

        updateColors();
    }

    bool acceptsPointer() const override { return true; }

    void updateForWindow(wm::Window *win)
    {
        if (!win || !win->state())
            return;
        m_zoomLabel->setText(win->state()->isMaximized() ? "Restore Size" : "Fill Screen");
        m_fullScreenLabel->setText(win->state()->isFullscreen() ? "Exit Full Screen" : "Enter Full Screen");
    }

    void updateColors()
    {
        const bool dark = ui::Theme::current().darkMode;
        const ui::Color headerColor = dark ? ui::Color(150, 150, 156, 255) : ui::Color(110, 110, 116, 255);
        const ui::Color textColor = dark ? ui::Color(238, 238, 242, 255) : ui::Color(36, 36, 40, 255);

        m_header1->setColor(headerColor);
        m_header2->setColor(headerColor);
        m_zoomLabel->setColor(textColor);
        m_fullScreenLabel->setColor(textColor);
    }

    void setHoveredItem(TileAction action)
    {
        if (m_hoveredAction == action)
            return;
        m_hoveredAction = action;
        requestPaint();
    }

protected:
    void pointerEnter() override
    {
        ui::View::pointerEnter();
        if (m_popover)
            m_popover->cancelDismiss();
    }

    void pointerMove(const ui::PointerEvent &event) override
    {
        ui::View::pointerMove(event);
        if (m_popover)
            m_popover->cancelDismiss();
        const TileAction hit = actionAt(event.position);
        setHoveredItem(hit);
    }

    void pointerLeave() override
    {
        ui::View::pointerLeave();
        setHoveredItem(TileAction::None);
        if (m_popover)
            m_popover->startDismiss();
    }

    void pointerRelease(const ui::PointerEvent &event) override
    {
        if (event.button != ui::MouseButton::Left)
            return;
        const TileAction hit = actionAt(event.position);
        if (hit != TileAction::None)
            m_popover->executeAction(hit);
    }

    void paint(ui::Painter &painter) override
    {
        updateColors();
        const bool dark = ui::Theme::current().darkMode;
        const ui::RectF rect = localRect();

        // Background card
        const ui::Color bg = dark ? ui::Color(32, 32, 36, 242) : ui::Color(246, 246, 248, 242);
        const ui::Color border = dark ? ui::Color(255, 255, 255, 34) : ui::Color(0, 0, 0, 26);
        const ui::Color dividerColor = dark ? ui::Color(255, 255, 255, 22) : ui::Color(0, 0, 0, 18);
        const ui::Color hoverBg = dark ? ui::Color(255, 255, 255, 26) : ui::Color(0, 0, 0, 16);
        const ui::Color activeBlue = ui::Color(0, 122, 255, 245);
        const ui::Color monitorBorder = dark ? ui::Color(130, 130, 136, 180) : ui::Color(155, 155, 162, 190);
        const ui::Color screenBg = dark ? ui::Color(46, 46, 52, 180) : ui::Color(228, 228, 234, 190);
        const ui::Color iconColor = dark ? ui::Color(230, 230, 235, 220) : ui::Color(45, 45, 50, 220);

        // Body squircle with border
        painter.drawSmoothRectangle(rect, 14.0, bg, 1.0, border);

        // Dividers
        painter.drawHorizontalLine(14, 72, rect.width - 28, dividerColor);
        painter.drawHorizontalLine(14, 138, rect.width - 28, dividerColor);

        // Helper to draw miniature monitor screen with high fidelity
        const auto drawMiniScreen = [&](const ui::RectF &slot, TileAction act, double subX, double subY, double subW, double subH) {
            const bool isHovered = (m_hoveredAction == act);
            if (isHovered)
                painter.drawSmoothRectangle(slot, 6.0, hoverBg, 0.5, border);

            // Screen container inside slot: 36 x 25
            const ui::RectF screen {slot.x + (slot.width - 36) / 2.0, slot.y + (slot.height - 25) / 2.0, 36.0, 25.0};
            const ui::Color activeBezel = isHovered ? ui::Color(0, 122, 255, 180) : monitorBorder;
            painter.drawSmoothRectangle(screen, 3.5, screenBg, 1.0, activeBezel);

            // Inactive regions (desktop tile backplate)
            const ui::RectF inner {screen.x + 1.5, screen.y + 1.5, 33.0, 22.0};
            painter.drawSmoothRectangle(inner, 2.0, dark ? ui::Color(255, 255, 255, 14) : ui::Color(0, 0, 0, 10));

            // Highlighted blue active tile
            const ui::RectF fillRect {inner.x + subX * inner.width + (subX > 0 ? 0.5 : 0),
                                      inner.y + subY * inner.height + (subY > 0 ? 0.5 : 0),
                                      subW * inner.width - (subW < 1.0 ? 0.5 : 0),
                                      subH * inner.height - (subH < 1.0 ? 0.5 : 0)};
            const ui::Color blue = isHovered ? ui::Color(10, 132, 255, 255) : activeBlue;
            painter.drawSmoothRectangle(fillRect, 2.0, blue);
        };

        // Draw 4 halves
        drawMiniScreen({14, 28, 48, 38}, TileAction::LeftHalf, 0.0, 0.0, 0.5, 1.0);
        drawMiniScreen({68, 28, 48, 38}, TileAction::RightHalf, 0.5, 0.0, 0.5, 1.0);
        drawMiniScreen({122, 28, 48, 38}, TileAction::TopHalf, 0.0, 0.0, 1.0, 0.5);
        drawMiniScreen({176, 28, 48, 38}, TileAction::BottomHalf, 0.0, 0.5, 1.0, 0.5);

        // Draw 4 quarters
        drawMiniScreen({14, 94, 48, 38}, TileAction::TopLeft, 0.0, 0.0, 0.5, 0.5);
        drawMiniScreen({68, 94, 48, 38}, TileAction::TopRight, 0.5, 0.0, 0.5, 0.5);
        drawMiniScreen({122, 94, 48, 38}, TileAction::BottomLeft, 0.0, 0.5, 0.5, 0.5);
        drawMiniScreen({176, 94, 48, 38}, TileAction::BottomRight, 0.5, 0.5, 0.5, 0.5);

        // Hover washes for action rows
        const ui::RectF row1 {10, 144, rect.width - 20, 28};
        if (m_hoveredAction == TileAction::Zoom)
            painter.drawSmoothRectangle(row1, 6.0, hoverBg);

        const ui::RectF row2 {10, 176, rect.width - 20, 28};
        if (m_hoveredAction == TileAction::FullScreen)
            painter.drawSmoothRectangle(row2, 6.0, hoverBg);

        // Draw Zoom / Fill Screen / Restore Size icon
        const bool isMax = m_popover->m_targetWindow && m_popover->m_targetWindow->state() && m_popover->m_targetWindow->state()->isMaximized();
        // Display frame
        painter.drawRoundedRect(ui::RectF(16, 149, 16, 11), {2.0, 2.0, ui::Corners::Circular}, iconColor, 1.2);
        // Stand base
        painter.drawLine({24, 160}, {24, 163}, iconColor, 1.2);
        painter.drawLine({21, 163}, {27, 163}, iconColor, 1.2);
        if (isMax) {
            // Restored floating window
            painter.drawRoundedRect(ui::RectF(19, 152, 10, 5), {1.0, 1.0, ui::Corners::Circular}, iconColor, 1.0);
        } else {
            // Filled window
            painter.drawSmoothRectangle(ui::RectF(18, 151, 12, 7), 1.0, iconColor);
        }

        // Draw Full Screen icon
        const bool isFs = m_popover->m_targetWindow && m_popover->m_targetWindow->state() && m_popover->m_targetWindow->state()->isFullscreen();
        if (isFs) {
            // Opposing diagonal arrows pointing inward (Exit Full Screen)
            painter.drawLine({25, 183}, {21, 187}, iconColor, 1.3);
            painter.drawLine({21, 184}, {21, 187}, iconColor, 1.3);
            painter.drawLine({21, 187}, {24, 187}, iconColor, 1.3);
            painter.drawLine({19, 197}, {23, 193}, iconColor, 1.3);
            painter.drawLine({23, 196}, {23, 193}, iconColor, 1.3);
            painter.drawLine({23, 193}, {20, 193}, iconColor, 1.3);
        } else {
            // Opposing diagonal arrows pointing outward (Enter Full Screen)
            painter.drawLine({17, 189}, {23, 183}, iconColor, 1.3);
            painter.drawLine({19, 183}, {23, 183}, iconColor, 1.3);
            painter.drawLine({23, 183}, {23, 187}, iconColor, 1.3);
            painter.drawLine({27, 191}, {21, 197}, iconColor, 1.3);
            painter.drawLine({25, 197}, {21, 197}, iconColor, 1.3);
            painter.drawLine({21, 197}, {21, 193}, iconColor, 1.3);
        }
    }

private:
    TileAction actionAt(const ui::PointF &pos) const
    {
        // Halves
        if (ui::RectF(14, 28, 48, 38).contains(pos)) return TileAction::LeftHalf;
        if (ui::RectF(68, 28, 48, 38).contains(pos)) return TileAction::RightHalf;
        if (ui::RectF(122, 28, 48, 38).contains(pos)) return TileAction::TopHalf;
        if (ui::RectF(176, 28, 48, 38).contains(pos)) return TileAction::BottomHalf;

        // Quarters
        if (ui::RectF(14, 94, 48, 38).contains(pos)) return TileAction::TopLeft;
        if (ui::RectF(68, 94, 48, 38).contains(pos)) return TileAction::TopRight;
        if (ui::RectF(122, 94, 48, 38).contains(pos)) return TileAction::BottomLeft;
        if (ui::RectF(176, 94, 48, 38).contains(pos)) return TileAction::BottomRight;

        // Action rows
        if (ui::RectF(10, 144, 216, 28).contains(pos)) return TileAction::Zoom;
        if (ui::RectF(10, 176, 216, 28).contains(pos)) return TileAction::FullScreen;

        return TileAction::None;
    }

    WindowTilePopover *m_popover;
    TileAction m_hoveredAction = TileAction::None;
    ui::Label *m_header1 = nullptr;
    ui::Label *m_header2 = nullptr;
    ui::Label *m_zoomLabel = nullptr;
    ui::Label *m_fullScreenLabel = nullptr;
};

WindowTilePopover::WindowTilePopover(Shell *shell)
    : m_shell(shell)
    , m_dismissTimer(core::mainLoop())
{
    m_dismissTimer.setInterval(std::chrono::milliseconds(250));
    m_dismissTimer.setSingleShot(true);
    m_dismissTimer.onTimeout = [this] {
        if (!m_buttonHovered && (!m_view || !m_view->isHovered()))
            hide();
    };
}

WindowTilePopover::~WindowTilePopover()
{
    m_window.reset();
}

void WindowTilePopover::notifyButtonHover(bool hovered)
{
    m_buttonHovered = hovered;
    if (hovered) {
        cancelDismiss();
    } else {
        if (m_window && m_window->isShown()) {
            startDismiss();
        }
    }
}

void WindowTilePopover::startDismiss()
{
    if (!m_buttonHovered && (!m_view || !m_view->isHovered())) {
        m_dismissTimer.start();
    }
}

void WindowTilePopover::cancelDismiss()
{
    m_dismissTimer.stop();
}

void WindowTilePopover::show(wm::Window *window, const base::PointF &buttonCenter)
{
    if (!window)
        return;

    m_targetWindow = window;

    if (!m_window) {
        m_window = std::make_unique<ShellWindow>(m_shell, wm::WindowRole::Picker);
        m_window->setFocusPolicy(wm::FocusPolicy::NoFocus);
        auto view = std::make_unique<PopoverView>(this);
        m_view = view.get();
        m_window->setContentView(std::move(view));
    }

    m_view->updateForWindow(window);

    constexpr int kWidth = 236;
    constexpr int kHeight = 214;

    const int x = int(std::round(buttonCenter.x() - 22));
    const int y = int(std::round(buttonCenter.y() + 16));

    m_window->setGeometry(base::Rect(x, y, kWidth, kHeight));
    m_window->setBackgroundBlur(base::Rect(0, 0, kWidth, kHeight), 14);
    m_window->show();
    cancelDismiss();
}

void WindowTilePopover::hide()
{
    m_dismissTimer.stop();
    m_targetWindow = nullptr;
    if (m_window)
        m_window->hide();
}

bool WindowTilePopover::isShown() const
{
    return m_window && m_window->isShown();
}

void WindowTilePopover::executeAction(TileAction action)
{
    if (!m_targetWindow || !m_targetWindow->manager()) {
        hide();
        return;
    }

    wm::Window *win = m_targetWindow;
    wm::WindowManager *mgr = win->manager();
    const base::Rect workArea = mgr->workAreaFor(win);

    switch (action) {
    case TileAction::LeftHalf:
        win->snapPrimary();
        break;
    case TileAction::RightHalf:
        win->snapSecondary();
        break;
    case TileAction::TopHalf: {
        if (!win->state()->isNormal())
            win->restore();
        const base::Rect bounds(workArea.x(), workArea.y(), workArea.width(), workArea.height() / 2);
        win->setBounds(bounds);
        break;
    }
    case TileAction::BottomHalf: {
        if (!win->state()->isNormal())
            win->restore();
        const int halfH = workArea.height() / 2;
        const base::Rect bounds(workArea.x(), workArea.y() + halfH, workArea.width(), workArea.height() - halfH);
        win->setBounds(bounds);
        break;
    }
    case TileAction::TopLeft: {
        if (!win->state()->isNormal())
            win->restore();
        const base::Rect bounds(workArea.x(), workArea.y(), workArea.width() / 2, workArea.height() / 2);
        win->setBounds(bounds);
        break;
    }
    case TileAction::TopRight: {
        if (!win->state()->isNormal())
            win->restore();
        const int halfW = workArea.width() / 2;
        const base::Rect bounds(workArea.x() + halfW, workArea.y(), workArea.width() - halfW, workArea.height() / 2);
        win->setBounds(bounds);
        break;
    }
    case TileAction::BottomLeft: {
        if (!win->state()->isNormal())
            win->restore();
        const int halfH = workArea.height() / 2;
        const base::Rect bounds(workArea.x(), workArea.y() + halfH, workArea.width() / 2, workArea.height() - halfH);
        win->setBounds(bounds);
        break;
    }
    case TileAction::BottomRight: {
        if (!win->state()->isNormal())
            win->restore();
        const int halfW = workArea.width() / 2;
        const int halfH = workArea.height() / 2;
        const base::Rect bounds(workArea.x() + halfW, workArea.y() + halfH, workArea.width() - halfW, workArea.height() - halfH);
        win->setBounds(bounds);
        break;
    }
    case TileAction::Zoom:
        if (win->state()->isMaximized())
            win->restore();
        else
            win->maximize();
        break;
    case TileAction::FullScreen:
        win->setFullscreen(!win->state()->isFullscreen());
        break;
    case TileAction::None:
        break;
    }

    hide();
}

} // namespace cutefish::shell
