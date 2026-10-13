// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WM_TYPES_H_
#define CUTEFISH_WM_TYPES_H_

#include <QRect>

namespace cutefish::wm {

enum class WindowType {
    Normal,
    Popup,
};

// The window states Cutefish supports. Ash's chromeos::WindowStateType has
// floated/pip/pinned as well; those are deliberately absent. The two snapped
// states are Ash's kPrimarySnapped / kSecondarySnapped: in the landscape layout
// Cutefish supports they are the left and the right half of the work area.
enum class WindowStateType {
    Normal,
    PrimarySnapped,
    SecondarySnapped,
    Maximized,
    Fullscreen,
    Minimized,
};

constexpr bool isSnappedStateType(WindowStateType type)
{
    return type == WindowStateType::PrimarySnapped || type == WindowStateType::SecondarySnapped;
}

// A snapped window keeps the floating look: rounded corners, outline and
// shadow. Only maximized and fullscreen windows are square.
constexpr bool isRoundedStateType(WindowStateType type)
{
    return type == WindowStateType::Normal || isSnappedStateType(type);
}

// What a window is for. Everything but Normal is a surface the trusted shell
// claimed through cutefish_shell_v1; the window manager only ever sees the
// adapted concept, never the protocol object.
enum class WindowRole {
    Normal,
    Desktop,
    Panel,
    Dock,
    Launcher,
    Notification,
    // The screen capture interface; never painted into a capture.
    Capture,
    // The on-screen display of a volume or brightness key; never focusable.
    Osd,
    // A shell dialog hanging from an application window, window-modal for it.
    Sheet,
    // A chooser, such as the clipboard history; only borrows the keyboard.
    Picker,
};

// Whether a window may take the keyboard. A Panel or Dock still receives
// pointer input: only activation is refused, so clicking the dock does not
// take the keyboard away from the application behind it.
enum class FocusPolicy {
    Normal,
    NoFocus,
};

// Panel/Dock stacking against ordinary windows. Ignored for other roles.
enum class PanelBehavior {
    StayAbove,
    WindowsCanCover,
};

// The screen edge an exclusive zone is measured from.
enum class ScreenEdge {
    Top,
    Bottom,
    Left,
    Right,
};

// Which edge or corner an interactive resize is anchored to. Cutefish uses its
// own enum rather than Chromium's HT* hit-test constants.
enum class ResizeEdge {
    None,
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

constexpr bool isLeftEdge(ResizeEdge edge)
{
    return edge == ResizeEdge::Left || edge == ResizeEdge::TopLeft
        || edge == ResizeEdge::BottomLeft;
}

constexpr bool isRightEdge(ResizeEdge edge)
{
    return edge == ResizeEdge::Right || edge == ResizeEdge::TopRight
        || edge == ResizeEdge::BottomRight;
}

constexpr bool isTopEdge(ResizeEdge edge)
{
    return edge == ResizeEdge::Top || edge == ResizeEdge::TopLeft
        || edge == ResizeEdge::TopRight;
}

constexpr bool isBottomEdge(ResizeEdge edge)
{
    return edge == ResizeEdge::Bottom || edge == ResizeEdge::BottomLeft
        || edge == ResizeEdge::BottomRight;
}

constexpr bool changesWidth(ResizeEdge edge)
{
    return isLeftEdge(edge) || isRightEdge(edge);
}

constexpr bool changesHeight(ResizeEdge edge)
{
    return isTopEdge(edge) || isBottomEdge(edge);
}

// The four edges a server-side frame occupies around the client area. All
// zero for a client-decorated or fullscreen window.
struct Insets {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool isNull() const { return left == 0 && top == 0 && right == 0 && bottom == 0; }

    // Outer rect -> client area.
    QRect inset(const QRect& rect) const
    {
        return rect.adjusted(left, top, -right, -bottom);
    }

    // Client area -> outer rect.
    QRect outset(const QRect& rect) const
    {
        return rect.adjusted(-left, -top, right, bottom);
    }

    QSize outset(const QSize& size) const
    {
        return QSize(size.width() + left + right, size.height() + top + bottom);
    }

    QSize inset(const QSize& size) const
    {
        return QSize(size.width() - left - right, size.height() - top - bottom);
    }

    QPoint topLeft() const { return QPoint(left, top); }

    friend bool operator==(const Insets& a, const Insets& b)
    {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
};

const char* toString(WindowStateType type);
const char* toString(WindowRole role);
const char* toString(ResizeEdge edge);

} // namespace cutefish::wm

#endif // CUTEFISH_WM_TYPES_H_
