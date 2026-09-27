// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modelled on chromium/src/components/exo/extended_drag_source.* (revision
// 7acd40ddefff49c9d36144999aafdbb674dbd07a), for xdg_toplevel_drag_v1 instead
// of zcr_extended_drag_v1.
//
// What is taken: a window attached before it maps is put under the pointer at
// its offset when it does, without an open animation; the window moves through
// the same interactive move as a caption drag, fed by the data-device drag
// that holds the pointer; it takes no part in picking the drop target; a drop
// ends the move where it is and a cancel reverts it; unmapping the window
// detaches it. What is dropped: cursor locking, the pending start raced
// against the attach, and the tab-dragging window property.
//
// What is added: a window dropped on another window of its own process, and
// unmapped by its client for it, hands the activation to that window. Chrome
// activates the tab strip it merges into itself, but Qt spends the pointer
// press serial on showing the dragged window, so the client cannot.

#ifndef CUTEFISH_WM_TOPLEVEL_DRAG_H_
#define CUTEFISH_WM_TOPLEVEL_DRAG_H_

#include <memory>
#include <optional>

#include <QObject>
#include <QPoint>

#include "compositor/surface_observer.h"
#include "input/drag.h"
#include "scene/window_animation.h"

namespace cutefish::clipboard {
class DataSource;
}

namespace cutefish::wm {

class Window;
class WindowManager;

class ToplevelDrag : public QObject,
                     public compositor::SurfaceObserver,
                     public input::DragWindowDelegate
{
    Q_OBJECT
public:
    ToplevelDrag(WindowManager *manager, std::shared_ptr<clipboard::DataSource> source);
    ~ToplevelDrag() override;

    // Whether a toplevel is attached that has not been unmapped since.
    bool hasToplevel() const { return m_surface != nullptr; }

    // Whether the drag this source started is still running.
    bool isDragging() const { return m_dragging; }

    // `offset` is the pointer's place in the window geometry of `surface`,
    // which is a toplevel's root surface, mapped or not.
    void attach(compositor::Surface *surface, const QPoint &offset);

    // --- input::DragWindowDelegate ---

    void dragMoved(const QPointF &location) override;
    void dragEnded(bool released) override;

    // --- compositor::SurfaceObserver ---

    void onSurfaceDestroying(compositor::Surface *surface) override;

private:
    void onDragStarted();
    void onWindowAdded(Window *window);
    void onWindowRemoved(Window *window);
    Window *windowForSurface() const;
    void startMove(Window *window);
    void stopMove();
    void detach();

    WindowManager *m_manager = nullptr;
    std::shared_ptr<clipboard::DataSource> m_source;
    compositor::Surface *m_surface = nullptr;
    QPoint m_offset;
    bool m_dragging = false;
    Window *m_moving = nullptr;
    // The window a released drag was dropped on, while the dragged one is
    // still mapped. Zero otherwise.
    quint64 m_dropWindow = 0;
    std::optional<scene::WindowAnimation::Style> m_animationStyle;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_TOPLEVEL_DRAG_H_
