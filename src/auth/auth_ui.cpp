// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/auth_ui.h"
#include "auth/auth_controller.h"
#include "auth/avatar_provider.h"
#include "auth/fallback_panel.h"
#include "auth/greetd_backend.h"
#include "auth/pam_backend.h"
#include "auth/rounded_rectangle.h"
#include "core/logging.h"
#include "core/process_environment.h"
#include "input/xkb_tracker.h"
#include "scene/node.h"
#include "session/session_controller.h"
#include <QCoreApplication>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static void initializeAuthResources()
{
    Q_INIT_RESOURCE(auth_qml);
    static const bool registered = [] {
        qmlRegisterType<cutefish::auth::RoundedRectangle>("Cutefish.Auth", 1, 0, "RoundedRectangle");
        qmlRegisterUncreatableType<cutefish::auth::RoundedRectangleBorder>(
                "Cutefish.Auth", 1, 0, "RoundedRectangleBorder",
                QStringLiteral("The border of a RoundedRectangle"));
        return true;
    }();
    Q_UNUSED(registered);
}

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

    void releaseRenderResources() override { m_texture.reset(); }

protected:
    void paint(render::Renderer &renderer, const QRect &bounds) const override
    {
        const QImage source = m_ui->renderImage();
        if (source.isNull()) {
            m_texture.reset();
            return;
        }
        // The lock transition draws every frame; the panel changes far less often.
        if (!m_texture || m_uploaded != m_ui->imageGeneration()) {
            // The renderer uploads the texture's own size and cannot see the
            // image's, so a panel redrawn for another output needs a new one.
            if (!m_texture || m_texture->size() != source.size()
                || !renderer.updateTexture(m_texture.get(), source.constBits(),
                                           source.bytesPerLine(), render::PixelFormat::Rgba8888))
                m_texture = renderer.createTexture(source.size(), source.constBits(),
                                                   source.bytesPerLine(),
                                                   render::PixelFormat::Rgba8888);
            m_uploaded = m_ui->imageGeneration();
        }
        if (!m_texture)
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
            renderer.drawTextureMapped(m_texture.get(), bounds, sample.panelOpacity, QTransform());
            return;
        }
        renderer.drawTextureTransformed(m_texture.get(), bounds, transform, sample.panelOpacity);
    }

private:
    AuthUi *m_ui;
    const session::SecureContainer *m_secure;
    mutable std::unique_ptr<render::Texture> m_texture;
    mutable quint64 m_uploaded = 0;
};

int qtKey(uint32_t symbol)
{
    switch (symbol) {
    case XKB_KEY_Return:
        return Qt::Key_Return;
    case XKB_KEY_KP_Enter:
        return Qt::Key_Enter;
    case XKB_KEY_BackSpace:
        return Qt::Key_Backspace;
    case XKB_KEY_Delete:
        return Qt::Key_Delete;
    case XKB_KEY_Tab:
        return Qt::Key_Tab;
    case XKB_KEY_ISO_Left_Tab:
        return Qt::Key_Backtab;
    case XKB_KEY_Escape:
        return Qt::Key_Escape;
    case XKB_KEY_Left:
        return Qt::Key_Left;
    case XKB_KEY_Right:
        return Qt::Key_Right;
    case XKB_KEY_Home:
        return Qt::Key_Home;
    case XKB_KEY_End:
        return Qt::Key_End;
    default:
        const char32_t unicode = xkb_keysym_to_utf32(symbol);
        return unicode ? QString::fromUcs4(&unicode, 1).toUpper().at(0).unicode() : Qt::Key_unknown;
    }
}
} // namespace

void AuthUi::configurePlatform()
{
    // Private: an input method's own settings window inherited the offscreen
    // platform, opened nowhere, and held its single-instance name.
    setPrivateEnvironment("QT_QPA_PLATFORM", "offscreen");
    setPrivateEnvironment("QT_QPA_OFFSCREEN_NO_GLX", "1");
    setPrivateEnvironment("QT_QPA_PLATFORMTHEME", "offscreen");
    setPrivateEnvironment("QT_QPA_GENERIC_PLUGINS", "");
    setPrivateEnvironment("QT_IM_MODULE", "none");
    setPrivateEnvironment("QT_QUICK_BACKEND", "software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
}

AuthUi::AuthUi(session::SessionController *session, const input::XkbTracker *xkb,
               AuthBackend *backend, const QUrl &source, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_xkb(xkb)
    , m_injectedBackend(backend)
    , m_source(source)
{
    initializeAuthResources();
    m_session->addObserver(this);
    m_session->setAuthInput(this);
    m_refresh.setSingleShot(true);
    connect(&m_refresh, &QTimer::timeout, this, &AuthUi::refresh);
    scheduleRefresh();
    connect(&m_repeatTimer, &QTimer::timeout, this, [this] {
        if (!m_session->isBlocked() || !m_window || !m_repeatFocus
            || m_window->activeFocusItem() != m_repeatFocus || !m_repeatFocus->isEnabled()
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
    m_repeatFocus.clear();
    m_started = false;
    if (m_node) {
        m_node->parent()->removeChild(m_node);
        m_node = nullptr;
    }
    if (m_control)
        m_control->invalidate();
    if (m_window)
        m_window->setRenderTarget({});
    m_root = nullptr;
    m_fallback.reset();
    m_window.reset();
    m_control.reset();
    m_engine.reset();
    m_controller.reset();
    m_backend.reset();
    m_image = {};
    m_buttons = {};
    m_attempted = false;
    m_authenticated = false;
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
        m_engine = std::make_unique<QQmlEngine>();
        m_engine->addImageProvider(QStringLiteral("cutefish-avatar"), new AvatarProvider);
        m_engine->rootContext()->setContextProperty(QStringLiteral("authController"),
                                                    m_controller.get());
        m_control = std::make_unique<QQuickRenderControl>();
        m_window = std::make_unique<QQuickWindow>(m_control.get());
        m_window->setColor(Qt::transparent);
        connect(m_window.get(), &QQuickWindow::activeFocusItemChanged, &m_repeatTimer,
                &QTimer::stop);
        // While the session is active the panel only has to be ready for the
        // next lock: drawn once. Later changes wait for the first locked frame.
        auto dirty = [this] {
            m_dirty = true;
            if (m_session->isBlocked() || m_image.isNull())
                scheduleRefresh();
        };
        connect(m_control.get(), &QQuickRenderControl::renderRequested, this, dirty);
        connect(m_control.get(), &QQuickRenderControl::sceneChanged, this, dirty);
        QQmlComponent component(
                m_engine.get(),
                m_source.isEmpty() ? QUrl(QStringLiteral("qrc:/auth/AuthScreen.qml")) : m_source);
        QObject *object = component.create();
        m_root = qobject_cast<QQuickItem *>(object);
        if (m_root) {
            m_root->setParent(m_window->contentItem());
            m_root->setParentItem(m_window->contentItem());
            QMetaObject::invokeMethod(m_root, "focusPassword");
            m_focused = true;
        } else {
            qCWarning(cfCompositor).noquote()
                    << "The lock screen failed to load:" << component.errorString();
            delete object;
            m_window.reset();
            m_control.reset();
            m_engine.reset();
            m_fallback = std::make_unique<FallbackPanel>(m_controller.get());
            connect(m_fallback.get(), &FallbackPanel::changed, this, dirty);
        }
        m_node = secure->authContainer()->addChild(std::make_unique<AuthNode>(this, secure));
    }
    if (!m_root && !m_fallback)
        return;
    if (m_session->isBlocked() && !m_started) {
        m_started = true;
        m_controller->start();
    }
    // A focused field blinks its caret, which would wake the compositor twice
    // a second for a panel no one can see.
    if (m_root && m_focused != m_session->isBlocked()) {
        m_focused = m_session->isBlocked();
        QFocusEvent focus(m_focused ? QEvent::FocusIn : QEvent::FocusOut,
                          Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(m_window.get(), &focus);
    }
    if (m_root)
        m_root->setProperty("blocked", m_session->isBlocked());
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
    const QSize pixels(qRound(size.width() * scale), qRound(size.height() * scale));
    if (m_image.size() != pixels || m_image.devicePixelRatio() != scale) {
        if (m_window)
            m_window->setRenderTarget({});
        // The texture's own byte order, so the panel uploads without a conversion.
        m_image = QImage(pixels, QImage::Format_RGBA8888_Premultiplied);
        m_image.setDevicePixelRatio(scale);
        m_image.fill(Qt::transparent);
        if (m_root) {
            m_window->setGeometry(QRect(QPoint(), size));
            QQuickRenderTarget target = QQuickRenderTarget::fromPaintDevice(&m_image);
            target.setDevicePixelRatio(scale);
            m_window->setRenderTarget(target);
            // A window never shown gets no resize event, so nothing sizes its content item.
            m_window->contentItem()->setSize(size);
            m_root->setSize(size);
            QEvent activate(QEvent::WindowActivate);
            QCoreApplication::sendEvent(m_window.get(), &activate);
        }
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
    if (!m_root || !m_root->isVisible())
        return {};
    const auto *pill = m_root->findChild<QQuickItem *>(QStringLiteral("passwordPill"));
    if (!pill || !pill->isVisible())
        return {};
    return pill->mapRectToItem(m_root, QRectF(0, 0, pill->width(), pill->height()));
}

QImage AuthUi::renderImage()
{
    if (m_resetPending || (!m_root && !m_fallback) || m_image.isNull() || !m_node
        || !m_node->isVisible())
        return {};
    if (m_dirty) {
        m_dirty = false;
        if (m_fallback) {
            m_image.fill(Qt::transparent);
            QPainter painter(&m_image);
            m_fallback->paint(painter, m_image.deviceIndependentSize().toSize());
        } else {
            m_control->polishItems();
            m_control->sync();
            m_control->render();
        }
        ++m_imageGeneration;
    }
    return m_image;
}

bool AuthUi::onKey(input::Timestamp, uint32_t code, bool pressed)
{
    if (!pressed && code == m_repeatCode)
        m_repeatTimer.stop();
    if (!sendKey(code, pressed, false) || m_fallback)
        return true;
    if (pressed && m_keyRepeat.enabled && m_xkb->keyRepeats(code) && !m_controller->busy()
        && !m_controller->succeeded()) {
        m_repeatCode = code;
        m_repeatFocus = m_window->activeFocusItem();
        m_repeatTimer.start(m_keyRepeat.delay);
    }
    return true;
}

bool AuthUi::sendKey(uint32_t code, bool pressed, bool repeat)
{
    if (!m_session->isBlocked() || m_resetPending || (!m_root && !m_fallback) || !m_xkb
        || !m_node->isEffectivelyVisible())
        return false;
    const uint32_t symbol = m_xkb->keysym(code);
    Qt::KeyboardModifiers modifiers;
    if (m_xkb->modifierActive(XKB_MOD_NAME_SHIFT))
        modifiers |= Qt::ShiftModifier;
    if (m_xkb->modifierActive(XKB_MOD_NAME_CTRL))
        modifiers |= Qt::ControlModifier;
    if (m_xkb->modifierActive(XKB_MOD_NAME_ALT))
        modifiers |= Qt::AltModifier;
    if (m_xkb->modifierActive(XKB_MOD_NAME_LOGO))
        modifiers |= Qt::MetaModifier;
    const char32_t unicode = xkb_keysym_to_utf32(symbol);
    const QString text = unicode >= 0x20 && !modifiers.testFlag(Qt::ControlModifier)
                    && !modifiers.testFlag(Qt::MetaModifier)
            ? QString::fromUcs4(&unicode, 1)
            : QString();
    if (m_fallback) {
        if (pressed)
            m_fallback->key(symbol, unicode == 0x7f ? QString() : text);
        return true;
    }
    QKeyEvent event(pressed ? QEvent::KeyPress : QEvent::KeyRelease, qtKey(symbol), modifiers,
                    code + 8, symbol, 0, text, repeat);
    QCoreApplication::sendEvent(m_window.get(), &event);
    return true;
}

void AuthUi::updateCursor()
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (m_root && m_node && m_node->isEffectivelyVisible()) {
        QPointF point = m_pointerPosition - m_node->absolutePosition();
        QQuickItem *item = m_root;
        while (item && item->contains(point) && item->isEnabled()) {
            if (item->cursor().shape() != Qt::ArrowCursor)
                shape = item->cursor().shape();
            auto *child = item->childAt(point.x(), point.y());
            if (!child)
                break;
            point = child->mapFromItem(item, point);
            item = child;
        }
    }
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
    if (!m_resetPending && m_window && m_root && m_node->isEffectivelyVisible()) {
        const QPointF local = position - m_node->absolutePosition();
        QMouseEvent event(QEvent::MouseMove, local, local, Qt::NoButton, m_buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window.get(), &event);
        updateCursor();
    }
    return true;
}

bool AuthUi::onPointerButton(input::Timestamp, const QPointF &position, uint32_t button,
                             bool pressed)
{
    if (m_resetPending || button != BTN_LEFT || !m_window || !m_root
        || !m_node->isEffectivelyVisible())
        return true;
    m_buttons.setFlag(Qt::LeftButton, pressed);
    const QPointF local = position - m_node->absolutePosition();
    QMouseEvent event(pressed ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, local, local,
                      Qt::LeftButton, m_buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window.get(), &event);
    return true;
}
} // namespace cutefish::auth
