// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/auth_ui.h"
#include "auth/auth_controller.h"
#include "auth/auth_view.h"
#include "auth/greetd_backend.h"
#include "auth/pam_backend.h"
#include "core/logging.h"
#include "input/xkb_tracker.h"
#include "scene/node.h"
#include "session/session_controller.h"
#include "toolkit/toolkit_host.h"
#include "ui/embedded_window.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

namespace cutefish::auth {
namespace {
class AuthNode final : public scene::Node
{
public:
    AuthNode(AuthUi *ui, const session::SecureContainer *secure)
        : Node(QStringLiteral("auth-panel"))
        , m_ui(ui)
        , m_secure(secure)
    {
    }

    Content content() const override { return Content::Translucent; }

    void releaseRenderResources() override { m_ui->releaseRenderResources(); }

protected:
    void paint(render::Renderer &renderer, const QRect &bounds) const override
    {
        // The lock transition draws every frame; the panel is rendered only
        // when it changed.
        render::Texture *texture = m_ui->panelTexture(renderer);
        if (!texture)
            return;
        const session::CoverSample sample = m_secure->sample(renderer);
        if (sample.panelOpacity <= 0)
            return;
        const QPointF center = QRectF(bounds).center();
        QTransform transform;
        transform.translate(center.x(), center.y());
        transform.scale(sample.panelScale, sample.panelScale);
        transform.translate(-center.x(), -center.y());
        // The blur behind the password field fades and scales with the panel.
        if (const QRectF field = m_ui->fieldBounds(); !field.isEmpty()) {
            render::GlassParams fade;
            fade.opacity = sample.panelOpacity;
            const QRectF blurred = transform.mapRect(field.translated(bounds.topLeft()));
            renderer.drawBackgroundBlur(blurred.toAlignedRect(), qRound(blurred.height() / 2),
                                        bounds, fade);
        }
        // Mapped at rest, so its device-density texels land on device pixels.
        if (sample.panelScale == 1) {
            renderer.drawTextureMapped(texture, bounds, sample.panelOpacity, QTransform());
            return;
        }
        renderer.drawTextureTransformed(texture, bounds, transform, sample.panelOpacity);
    }

private:
    AuthUi *m_ui;
    const session::SecureContainer *m_secure;
};

Qt::CursorShape qtCursor(ui::CursorShape shape)
{
    switch (shape) {
    case ui::CursorShape::Text:
        return Qt::IBeamCursor;
    case ui::CursorShape::Pointer:
        return Qt::PointingHandCursor;
    default:
        return Qt::ArrowCursor;
    }
}

uint32_t toolkitModifiers(const input::XkbTracker *xkb)
{
    uint32_t modifiers = ui::NoModifier;
    if (xkb->modifierActive(XKB_MOD_NAME_SHIFT))
        modifiers |= ui::ShiftModifier;
    if (xkb->modifierActive(XKB_MOD_NAME_CTRL))
        modifiers |= ui::ControlModifier;
    if (xkb->modifierActive(XKB_MOD_NAME_ALT))
        modifiers |= ui::AltModifier;
    if (xkb->modifierActive(XKB_MOD_NAME_LOGO))
        modifiers |= ui::SuperModifier;
    return modifiers;
}
} // namespace

AuthUi::AuthUi(session::SessionController *session, const input::XkbTracker *xkb,
               AuthBackend *backend, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_xkb(xkb)
    , m_injectedBackend(backend)
{
    m_session->addObserver(this);
    m_session->setAuthInput(this);
    m_refresh.setSingleShot(true);
    connect(&m_refresh, &QTimer::timeout, this, &AuthUi::refresh);
    scheduleRefresh();
    connect(&m_repeatTimer, &QTimer::timeout, this, [this] {
        if (!m_session->isBlocked() || !m_window || !m_repeatFocus
            || m_window->focusView() != m_repeatFocus || !m_repeatFocus->isEnabled()
            || m_session->state() == session::SessionState::Unlocking) {
            m_repeatTimer.stop();
            return;
        }
        m_repeatTimer.setInterval(m_keyRepeat.interval);
        sendKey(m_repeatCode, true, true);
    });
    // logind authenticates the sender and identifies this compositor's session.
    QDBusInterface manager(
            QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"), QDBusConnection::systemBus());
    QDBusReply<QDBusObjectPath> path =
            manager.call(QStringLiteral("GetSessionByPID"), uint(getpid()));
    if (path.isValid())
        QDBusConnection::systemBus().connect(QStringLiteral("org.freedesktop.login1"),
                                             path.value().path(),
                                             QStringLiteral("org.freedesktop.login1.Session"),
                                             QStringLiteral("Lock"), this, SLOT(requestLock()));
}

AuthUi::~AuthUi()
{
    m_session->setAuthInput(nullptr);
    m_session->removeObserver(this);
    clear();
}

void AuthUi::requestLock()
{
    m_session->requestLock();
}

void AuthUi::onSessionStateChanged(session::SessionState state)
{
    if (state == session::SessionState::Active || state == session::SessionState::Unlocking) {
        m_repeatTimer.stop();
        m_cursorShape = Qt::ArrowCursor;
        emit cursorShapeChanged(m_cursorShape);
    }
    if (state == session::SessionState::Active) {
        m_authenticated = false;
        m_resetPending = true;
    }
    // Authentication signals may still be on the stack when state changes.
    scheduleRefresh();
}

void AuthUi::onSessionOutputsChanged()
{
    scheduleRefresh();
}

void AuthUi::scheduleRefresh()
{
    if (!m_refresh.isActive())
        m_refresh.start(0);
}

void AuthUi::clear()
{
    m_repeatTimer.stop();
    m_repeatFocus = nullptr;
    m_started = false;
    if (m_node) {
        m_node->parent()->removeChild(m_node);
        m_node = nullptr;
    }
    m_screen = nullptr;
    m_panel.reset();
    toolkit::destroyWindow(m_window);
    m_controller.reset();
    m_backend.reset();
    m_sized = false;
    m_pointerInside = false;
    m_viewCursor = Qt::ArrowCursor;
    m_attempted = false;
    m_authenticated = false;
}

void AuthUi::setClockForTesting(std::time_t now)
{
    m_clockForTesting = now;
    if (m_screen)
        m_screen->setClockForTesting(now);
}

ui::View *AuthUi::focusView() const
{
    return m_window ? m_window->focusView() : nullptr;
}

void AuthUi::refresh()
{
    if (m_resetPending) {
        clear();
        m_resetPending = false;
    }
    if (m_authenticated && m_session->state() == session::SessionState::Locked)
        m_session->requestUnlock();
    if (m_session->state() == session::SessionState::Unlocking)
        return;
    auto *secure = m_session->secureContainer();
    if (!secure || secure->outputs().isEmpty()) {
        if (m_node)
            m_node->setVisible(false);
        return;
    }
    if (!m_attempted) {
        m_attempted = true;
        const bool login = m_session->state() == session::SessionState::Login;
        if (!m_injectedBackend) {
            if (login)
                m_backend = std::make_unique<GreetdBackend>();
            else
                m_backend = std::make_unique<PamBackend>();
        }
        m_controller = std::make_unique<AuthController>(
                login ? AuthController::Login : AuthController::Lock,
                m_injectedBackend ? m_injectedBackend : m_backend.get());
        connect(m_controller.get(), &AuthController::stateChanged, &m_repeatTimer, &QTimer::stop);
        connect(m_controller.get(), &AuthController::authenticationSucceeded, this, [this, login] {
            // greetd owns login handoff; the greeter stays covered until it exits.
            if (!login) {
                m_authenticated = true;
                m_session->requestUnlock();
            }
        });
        toolkit::application();
        m_window = std::make_unique<ui::EmbeddedWindow>();
        auto screen = std::make_unique<AuthScreen>(m_window.get(), m_controller.get());
        m_screen = screen.get();
        if (m_clockForTesting)
            m_screen->setClockForTesting(*m_clockForTesting);
        m_window->setContentView(std::move(screen));
        m_screen->focusPassword();
        m_panel = std::make_unique<toolkit::PanelTexture>(m_window.get());
        // While the session is active the panel only has to be ready for the
        // next lock: drawn once. Later changes wait for the first locked frame.
        m_window->onRenderRequested = [this] {
            m_dirty = true;
            if (m_session->isBlocked() || m_panel->image().isNull())
                scheduleRefresh();
        };
        m_window->onCursorShapeChanged = [this](ui::CursorShape shape) {
            m_viewCursor = qtCursor(shape);
            if (m_session->isBlocked())
                updateCursor();
        };
        m_focused = false;
        m_node = secure->authContainer()->addChild(std::make_unique<AuthNode>(this, secure));
    }
    if (m_session->isBlocked() && !m_started) {
        m_started = true;
        m_controller->start();
    }
    // A focused field blinks its caret, which would wake the compositor twice
    // a second for a panel no one can see.
    if (m_focused != m_session->isBlocked()) {
        m_focused = m_session->isBlocked();
        m_window->setKeyboardFocus(m_focused);
        m_window->setActive(m_focused);
        m_screen->setKeyboardFocused(m_focused);
    }
    m_screen->setBlocked(m_session->isBlocked());
    const session::SecureOutput &target = panelOutput(secure->outputs());
    m_panelOutput = target.name;
    const QRect output = target.bounds;
    const double scale = target.scale;
    // The output's full height: the clock at the top, the login column at the bottom.
    const QSize size(std::min(480, output.width()), output.height());
    // QRect::center() rounds down a pixel, which a panel as tall as the output
    // would overhang.
    const QRect bounds(output.topLeft()
                               + QPoint((output.width() - size.width()) / 2,
                                        (output.height() - size.height()) / 2),
                       size);
    m_node->setGeometry(bounds);
    m_node->setVisible(!size.isEmpty());
    if (size.isEmpty())
        return;
    // Drawn at the output's density: an image of the logical size would be
    // stretched over the device pixels and come out blurred.
    if (!(m_window->size() == ui::SizeF {double(size.width()), double(size.height())})
        || m_scale != scale || !m_sized) {
        m_scale = scale;
        m_window->setScale(scale);
        m_window->setSize({double(size.width()), double(size.height())});
        m_sized = true;
        m_dirty = true;
    }
    if (m_dirty) {
        if (m_session->isBlocked())
            m_node->notifyChanged();
        else
            renderImage();
    }
    if (m_session->isBlocked())
        updateCursor();
}

const session::SecureOutput &
AuthUi::panelOutput(const QList<session::SecureOutput> &outputs) const
{
    // The screen the user is looking at: the one under the pointer, else the primary one.
    const QPointF pointer = m_pointerLocator ? m_pointerLocator() : m_pointerPosition;
    for (const session::SecureOutput &output : outputs) {
        if (QRectF(output.bounds).contains(pointer))
            return output;
    }
    for (const session::SecureOutput &output : outputs) {
        if (output.primary)
            return output;
    }
    return outputs.front();
}

QRectF AuthUi::fieldBounds() const
{
    if (!m_screen)
        return {};
    // Where the next frame puts it, also before the first one.
    m_window->updateLayout();
    return m_screen->fieldBounds();
}

QImage AuthUi::renderImage()
{
    if (m_resetPending || !m_window || !m_node || !m_node->isVisible())
        return {};
    if (m_dirty || m_panel->image().isNull()) {
        m_dirty = false;
        if (m_panel->renderImage())
            ++m_imageGeneration;
    }
    return m_panel->image();
}

render::Texture *AuthUi::panelTexture(render::Renderer &renderer)
{
    if (m_resetPending || !m_window || !m_node || !m_node->isVisible())
        return nullptr;
    const bool dirty = m_dirty;
    m_dirty = false;
    return m_panel->texture(renderer, dirty);
}

void AuthUi::releaseRenderResources()
{
    if (m_panel)
        m_panel->releaseRenderResources();
}

bool AuthUi::onKey(input::Timestamp, uint32_t code, bool pressed)
{
    if (!pressed && code == m_repeatCode)
        m_repeatTimer.stop();
    if (!sendKey(code, pressed, false))
        return true;
    if (pressed && m_keyRepeat.enabled && m_xkb->keyRepeats(code) && !m_controller->busy()
        && !m_controller->succeeded()) {
        m_repeatCode = code;
        m_repeatFocus = m_window->focusView();
        m_repeatTimer.start(m_keyRepeat.delay);
    }
    return true;
}

bool AuthUi::sendKey(uint32_t code, bool pressed, bool repeat)
{
    if (!m_session->isBlocked() || m_resetPending || !m_window || !m_xkb
        || !m_node->isEffectivelyVisible())
        return false;
    ui::KeyEvent event;
    event.key = m_xkb->keysym(code);
    event.keycode = code;
    event.modifiers = toolkitModifiers(m_xkb);
    event.repeat = repeat;
    const char32_t unicode = xkb_keysym_to_utf32(event.key);
    if (unicode >= 0x20 && unicode != 0x7f) {
        char text[8] = {};
        if (xkb_keysym_to_utf8(event.key, text, sizeof(text)) > 0)
            event.text = text;
    }
    m_window->key(event, pressed);
    return true;
}

void AuthUi::updateCursor()
{
    const Qt::CursorShape shape =
            m_window && m_node && m_node->isEffectivelyVisible() && m_pointerInside
            ? m_viewCursor
            : Qt::ArrowCursor;
    if (shape != m_cursorShape) {
        m_cursorShape = shape;
        emit cursorShapeChanged(shape);
    }
}

bool AuthUi::onPointerMotion(input::Timestamp, const QPointF &position)
{
    m_pointerPosition = position;
    const auto *secure = m_session->secureContainer();
    if (m_node && secure && !secure->outputs().isEmpty()
        && panelOutput(secure->outputs()).name != m_panelOutput)
        scheduleRefresh();
    if (!m_resetPending && m_window && m_node->isEffectivelyVisible()) {
        const QPointF local = position - m_node->absolutePosition();
        m_pointerInside = QRectF(QPointF(), QSizeF(m_node->geometry().size())).contains(local);
        // The views under the pointer are where the next frame puts them.
        m_window->updateLayout();
        m_window->pointerMove({local.x(), local.y()});
        updateCursor();
    }
    return true;
}

bool AuthUi::onPointerButton(input::Timestamp, const QPointF &position, uint32_t button,
                             bool pressed)
{
    if (m_resetPending || button != BTN_LEFT || !m_window || !m_node->isEffectivelyVisible())
        return true;
    const QPointF local = position - m_node->absolutePosition();
    m_window->updateLayout();
    m_window->pointerMove({local.x(), local.y()});
    m_window->pointerButton(ui::MouseButton::Left, pressed, m_xkb ? toolkitModifiers(m_xkb) : 0);
    return true;
}
} // namespace cutefish::auth
