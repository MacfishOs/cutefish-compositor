// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The Cutefish server-side window frame.
//
// Architecture follows ash/frame/frame_view_ash.* : one frame object per
// window, owning its caption buttons, its hit regions and the state it draws
// from, and turning a press into a window-state request or into an interactive
// move/resize on the existing window resizer. It does not own any geometry -
// wm::Window does - and it knows nothing about Wayland.
//
// The visual language is Cutefish's own; see decoration/decoration_theme.h.

#ifndef CUTEFISH_DECORATION_DECORATION_H_
#define CUTEFISH_DECORATION_DECORATION_H_

#include <memory>
#include <optional>
#include <vector>

#include <QPoint>
#include <QRect>

#include "decoration/caption_button.h"
#include "decoration/decoration_theme.h"
#include "wm/window_frame.h"
#include "wm/window_observer.h"

namespace cutefish::wm {
class Window;
}

namespace cutefish::decoration {

class DecorationNode;

class Decoration : public wm::WindowFrame, public wm::WindowObserver, public ThemeObserver
{
public:
    Decoration(wm::Window* window, Theme* theme);
    Decoration(const Decoration&) = delete;
    Decoration& operator=(const Decoration&) = delete;
    ~Decoration() override;

    wm::Window* window() const { return m_window; }
    Theme* theme() const { return m_theme; }

    // --- What the node draws ---

    bool isActive() const;
    // Exterior rounding is disabled when the window fills the work area.
    bool hasRoundedCorners() const;
    QString title() const;
    const std::vector<CaptionButton>& buttons() const { return m_buttons; }
    // Where the caption may be drawn, in frame coordinates.
    QRect captionArea() const;
    // True once the window is maximized, so the maximize button shows the
    // restore icon.
    bool isRestoreButton() const;

    // --- wm::WindowFrame ---

    wm::Insets extents() const override;
    QRect inputBounds() const override;
    wm::FrameArea areaAt(const QPointF& location) const override;
    input::CursorShape cursorShapeAt(const QPointF& location) const override;
    bool onPointerMotion(input::Timestamp timestamp, const QPointF& location) override;
    bool onPointerButton(input::Timestamp timestamp, const QPointF& location,
                         bool pressed) override;
    void onPointerLeave() override;
    bool isTrackingPress() const override { return m_pressedButton >= 0 && !m_pressTouch; }
    void cancelPress() override;
    QRect titleBarBounds() const override;
    std::vector<QRect> captionButtonBounds() const override;
    bool onTouchDown(input::Timestamp timestamp, int32_t id, const QPointF& location) override;
    bool onTouchMotion(input::Timestamp timestamp, int32_t id, const QPointF& location) override;
    bool onTouchUp(input::Timestamp timestamp, int32_t id) override;
    void onTouchCancel() override;

    // --- wm::WindowObserver ---

    void onWindowBoundsChanged(wm::Window* window, const QRect& oldBounds) override;
    void onWindowContentChanged(wm::Window* window) override
    { onWindowBoundsChanged(window, QRect()); }
    void onWindowFrameChanged(wm::Window* window) override
    { onWindowBoundsChanged(window, QRect()); }
    void onWindowStateTypeChanged(wm::Window* window, wm::WindowStateType oldType) override;
    void onWindowActivationChanged(wm::Window* window, bool active) override;
    void onWindowTitleChanged(wm::Window* window) override;
    void onWindowDestroying(wm::Window* window) override;

    // --- ThemeObserver ---

    void onThemeChanged() override { scheduleRepaint(); }

private:
    // Frame coordinates: the outer window bounds with their origin at (0, 0).
    QPoint toFrame(const QPointF& location) const;
    QRect frameRect() const;
    QRect titleBarRect() const;
    void layout();
    void updateNode();
    void scheduleRepaint();
    // -1 when the point is not on a button.
    int buttonIndexAt(const QPoint& framePoint) const;
    wm::ResizeEdge resizeEdgeAt(const QPoint& framePoint) const;
    // A pointer press, or a touch point `touchId` going down.
    bool press(input::Timestamp timestamp, const QPointF& location,
               std::optional<int32_t> touchId);
    // Ends the held button press at `framePoint`, running its action there.
    void releaseButton(const QPoint& framePoint);
    void runButtonAction(CaptionButtonType type);
    bool setHoveredButton(int index);
    void toggleMaximize();

    wm::Window* m_window = nullptr;
    Theme* m_theme = nullptr;
    DecorationNode* m_node = nullptr;

    // Right to left in the titlebar: close, maximize, minimize, which is the
    // order the Cutefish KWin configuration ships (ButtonsOnRight=HIAX).
    std::vector<CaptionButton> m_buttons;
    int m_hoveredButton = -1;
    int m_pressedButton = -1;
    // The touch point holding m_pressedButton, and where it is; empty while
    // the pointer holds it.
    std::optional<int32_t> m_pressTouch;
    QPoint m_pressTouchPoint;

    // For the double-click that toggles maximize.
    input::Timestamp m_lastTitleBarPress {};
    QPoint m_lastTitleBarPressPoint;
    bool m_hadTitleBarPress = false;
};

// Installed on the window manager; hands out Decorations.
class DecorationProvider : public wm::FrameProvider
{
public:
    explicit DecorationProvider(Theme* theme);

    std::unique_ptr<wm::WindowFrame> createFrame(wm::Window* window) override;
    wm::Insets defaultExtents() const override;

private:
    Theme* m_theme = nullptr;
};

} // namespace cutefish::decoration

#endif // CUTEFISH_DECORATION_DECORATION_H_
