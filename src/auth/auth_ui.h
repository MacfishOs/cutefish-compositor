// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_UI_H
#define CUTEFISH_AUTH_UI_H
#include "input/event_sink.h"
#include "session/secure_container.h"
#include "session/session_state.h"
#include <QImage>
#include <QObject>
#include <QTimer>
#include <ctime>
#include <optional>
#include <functional>
#include <memory>

namespace cutefish::input {
class XkbTracker;
}

namespace cutefish::render {
class Renderer;
class Texture;
} // namespace cutefish::render

namespace cutefish::toolkit {
class PanelTexture;
}

namespace cutefish::scene {
class Node;
}

namespace cutefish::ui {
class EmbeddedWindow;
class View;
} // namespace cutefish::ui

namespace cutefish::session {
class SessionController;
}

namespace cutefish::auth {
class AuthBackend;
class AuthController;
class AuthScreen;

class AuthUi : public QObject, public input::EventSink, public session::SessionObserver
{
    Q_OBJECT
public:
    AuthUi(session::SessionController *session, const input::XkbTracker *xkb,
           AuthBackend *backend = nullptr, QObject *parent = nullptr);
    ~AuthUi() override;
    void onSessionStateChanged(session::SessionState state) override;
    void onSessionOutputsChanged() override;
    bool onKey(input::Timestamp, uint32_t code, bool pressed) override;
    bool onPointerMotion(input::Timestamp, const QPointF &position) override;
    bool onPointerButton(input::Timestamp, const QPointF &position, uint32_t button,
                         bool pressed) override;

    // The panel's toolkit window, made with the controller on the first
    // refresh with an output.
    ui::EmbeddedWindow *window() const { return m_window.get(); }
    AuthScreen *screen() const { return m_screen; }
    // The view that has the keyboard, for tests.
    ui::View *focusView() const;

    AuthController *controller() const { return m_controller.get(); }

    // Whether a refresh() is queued. Nothing queues one while the panel is idle.
    bool isRefreshPending() const { return m_refresh.isActive(); }

    // The panel has its size for the output, so the next frame can show it.
    bool isPanelDrawn() const { return m_window && m_sized; }

    // Changes whenever renderImage() draws the panel anew.
    quint64 imageGeneration() const { return m_imageGeneration; }

    // Where the pointer is, for the panel to open on the screen it is on.
    void setPointerLocator(std::function<QPointF()> locator)
    {
        m_pointerLocator = std::move(locator);
    }

    void setKeyRepeatSettings(const input::KeyRepeatSettings &settings)
    {
        m_keyRepeat = settings;
    }

    // The password field in panel coordinates, for the backdrop blur behind it.
    QRectF fieldBounds() const;

    // The panel rendered on the CPU, when it changed.
    QImage renderImage();
    // In the panel node's paint: the panel as a texture of the renderer,
    // drawn on the GPU where the renderer allows.
    render::Texture *panelTexture(render::Renderer &renderer);
    void releaseRenderResources();
    void refresh();
    // Seconds since the epoch the clock shows instead of the time, for tests.
    void setClockForTesting(std::time_t now);
signals:
    void cursorShapeChanged(Qt::CursorShape shape);

public slots:
    void requestLock();

private:
    void clear();
    void scheduleRefresh();
    const session::SecureOutput &panelOutput(const QList<session::SecureOutput> &outputs) const;
    bool sendKey(uint32_t code, bool pressed, bool repeat);
    void updateCursor();
    session::SessionController *m_session;
    const input::XkbTracker *m_xkb;
    AuthBackend *m_injectedBackend;
    QTimer m_refresh;
    QTimer m_repeatTimer;
    input::KeyRepeatSettings m_keyRepeat;
    uint32_t m_repeatCode = 0;
    ui::View *m_repeatFocus = nullptr;
    QPointF m_pointerPosition;
    std::function<QPointF()> m_pointerLocator;
    QString m_panelOutput;
    Qt::CursorShape m_cursorShape = Qt::ArrowCursor;
    bool m_started = false;
    std::unique_ptr<AuthBackend> m_backend;
    std::unique_ptr<AuthController> m_controller;
    std::unique_ptr<ui::EmbeddedWindow> m_window;
    std::unique_ptr<toolkit::PanelTexture> m_panel;
    AuthScreen *m_screen = nullptr;
    // The cursor the panel's views ask for, shown while the session is blocked.
    Qt::CursorShape m_viewCursor = Qt::ArrowCursor;
    std::optional<std::time_t> m_clockForTesting;
    double m_scale = 1;
    bool m_sized = false;
    scene::Node *m_node = nullptr;
    bool m_pointerInside = false;
    bool m_attempted = false;
    bool m_authenticated = false;
    bool m_focused = false;
    bool m_resetPending = false;
    bool m_dirty = true;
    quint64 m_imageGeneration = 0;
};
} // namespace cutefish::auth
#endif
