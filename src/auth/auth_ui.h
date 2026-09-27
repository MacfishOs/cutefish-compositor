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
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
class QQmlEngine;
class QQuickRenderControl;
class QQuickWindow;
class QQuickItem;

namespace cutefish::input {
class XkbTracker;
}

namespace cutefish::render {
class Renderer;
}

namespace cutefish::scene {
class Node;
}

namespace cutefish::session {
class SessionController;
}

namespace cutefish::auth {
class AuthBackend;
class AuthController;
class FallbackPanel;

class AuthUi : public QObject, public input::EventSink, public session::SessionObserver
{
    Q_OBJECT
public:
    AuthUi(session::SessionController *session, const input::XkbTracker *xkb,
           AuthBackend *backend = nullptr, const QUrl &source = QUrl(), QObject *parent = nullptr);
    ~AuthUi() override;
    static void configurePlatform();
    void onSessionStateChanged(session::SessionState state) override;
    void onSessionOutputsChanged() override;
    bool onKey(input::Timestamp, uint32_t code, bool pressed) override;
    bool onPointerMotion(input::Timestamp, const QPointF &position) override;
    bool onPointerButton(input::Timestamp, const QPointF &position, uint32_t button,
                         bool pressed) override;

    QQuickWindow *window() const { return m_window.get(); }

    AuthController *controller() const { return m_controller.get(); }

    // Whether a refresh() is queued. Nothing queues one while the panel is idle.
    bool isRefreshPending() const { return m_refresh.isActive(); }

    // The panel has been rendered at least once, so the next frame can show it.
    bool isPanelDrawn() const { return !m_image.isNull(); }

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

    QImage renderImage();
    void refresh();
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
    QUrl m_source;
    QTimer m_refresh;
    QTimer m_repeatTimer;
    input::KeyRepeatSettings m_keyRepeat;
    uint32_t m_repeatCode = 0;
    QPointer<QQuickItem> m_repeatFocus;
    QPointF m_pointerPosition;
    std::function<QPointF()> m_pointerLocator;
    QString m_panelOutput;
    Qt::CursorShape m_cursorShape = Qt::ArrowCursor;
    bool m_started = false;
    std::unique_ptr<AuthBackend> m_backend;
    std::unique_ptr<AuthController> m_controller;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QQuickRenderControl> m_control;
    std::unique_ptr<QQuickWindow> m_window;
    QQuickItem *m_root = nullptr;
    std::unique_ptr<FallbackPanel> m_fallback;
    scene::Node *m_node = nullptr;
    QImage m_image;
    Qt::MouseButtons m_buttons;
    bool m_attempted = false;
    bool m_authenticated = false;
    bool m_focused = false;
    bool m_resetPending = false;
    bool m_dirty = true;
    quint64 m_imageGeneration = 0;
};
} // namespace cutefish::auth
#endif
