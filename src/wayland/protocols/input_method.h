// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <functional>
#include <memory>
#include <wayland-server-core.h>

namespace cutefish::scene {
class Node;
}

namespace cutefish::wayland {
struct WaylandSeat;

// Where a candidate popup of `size` goes for the text cursor `caret`, both in
// output coordinates: below the caret, above it when only there it fits, and
// kept inside `area`. Upstream: views::BubbleFrameView's
// adjust_if_offscreen, mirroring the arrow and then clamping to the work area.
QPoint inputPopupPosition(const QRect &caret, const QSize &size, const QRect &area);

// Owns the application/IME relay; resources must die before the seat and display.
class InputMethodManager
{
public:
    // Candidate popups are placed in `popupLayer`, in output coordinates, so they
    // stack above every window, including the application's own popups.
    InputMethodManager(wl_display *display, WaylandSeat *seat, scene::Node *popupLayer);
    ~InputMethodManager();
    InputMethodManager(const InputMethodManager &) = delete;
    InputMethodManager &operator=(const InputMethodManager &) = delete;

    // Keeps popups on the text cursor when windows move; call on scene changes.
    void sceneChanged();

    // Releases the globals; must run before the display is destroyed.
    void shutdown();

    // The work area a candidate popup stays inside, for a text cursor in output
    // coordinates. Unset, popups only follow the cursor.
    void setPlacementArea(std::function<QRect(const QRect &caret)> area);

    // Restricts input-method and virtual-keyboard binds; unset allows every client.
    void setAuthorizer(std::function<bool(const wl_client *)> authorizer);

    wl_global *textInputGlobal() const;
    wl_global *textInputV1Global() const;
    wl_global *inputMethodGlobal() const;
    wl_global *virtualKeyboardGlobal() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace cutefish::wayland
