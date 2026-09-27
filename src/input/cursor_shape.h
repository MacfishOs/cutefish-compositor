// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Cursor shapes used by the compositor and window manager.
#pragma once

#include <Qt>

namespace cutefish::input {

// Standard cursor-shape names. Hidden is internal, never a protocol shape.
enum class CursorShape {
    Hidden = 0,
    Default,
    ContextMenu,
    Help,
    Pointer,
    Progress,
    Wait,
    Cell,
    Crosshair,
    Text,
    VerticalText,
    Alias,
    Copy,
    Move,
    NoDrop,
    NotAllowed,
    Grab,
    Grabbing,
    EResize,
    NResize,
    NeResize,
    NwResize,
    SResize,
    SeResize,
    SwResize,
    WResize,
    EwResize,
    NsResize,
    NeswResize,
    NwseResize,
    ColResize,
    RowResize,
    AllScroll,
    ZoomIn,
    ZoomOut
};

inline CursorShape cursorShapeFromQt(Qt::CursorShape shape)
{
    switch (shape) {
    case Qt::BlankCursor:
        return CursorShape::Hidden;
    case Qt::CrossCursor:
        return CursorShape::Crosshair;
    case Qt::WaitCursor:
        return CursorShape::Wait;
    case Qt::IBeamCursor:
        return CursorShape::Text;
    case Qt::SizeVerCursor:
        return CursorShape::NsResize;
    case Qt::SizeHorCursor:
        return CursorShape::EwResize;
    case Qt::SizeBDiagCursor:
        return CursorShape::NeswResize;
    case Qt::SizeFDiagCursor:
        return CursorShape::NwseResize;
    case Qt::SizeAllCursor:
        return CursorShape::Move;
    case Qt::PointingHandCursor:
        return CursorShape::Pointer;
    case Qt::ForbiddenCursor:
        return CursorShape::NotAllowed;
    case Qt::WhatsThisCursor:
        return CursorShape::Help;
    case Qt::BusyCursor:
        return CursorShape::Progress;
    case Qt::OpenHandCursor:
        return CursorShape::Grab;
    case Qt::ClosedHandCursor:
        return CursorShape::Grabbing;
    case Qt::SplitHCursor:
        return CursorShape::ColResize;
    case Qt::SplitVCursor:
        return CursorShape::RowResize;
    case Qt::DragCopyCursor:
        return CursorShape::Copy;
    case Qt::DragMoveCursor:
        return CursorShape::Move;
    case Qt::DragLinkCursor:
        return CursorShape::Alias;
    default:
        return CursorShape::Default;
    }
}

} // namespace cutefish::input
