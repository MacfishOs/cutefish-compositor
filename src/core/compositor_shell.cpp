// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/compositor_shell.h"

#include <unistd.h>

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QFileInfo>
#include <QPointer>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUuid>

#include "accelerators/accelerator_configuration.h"
#include "accelerators/accelerator_controller.h"
#include "accelerators/accelerator_delegate.h"
#include "accelerators/accelerator_filter.h"
#include "accelerators/accelerator_service.h"
#include "backend/drm/drm_backend.h"
#include "backend/drm/drm_output.h"
#include "backend/headless/headless_backend.h"
#include "backend/input/libinput_backend.h"
#include "backend/session/libseat_session.h"
#include "compositor/surface.h"
#include "computer_control/computer_control_service.h"
#include "computer_control/controller.h"
#include "computer_control/permission_consent.h"
#include "core/computer_control_environment.h"
#include "core/logging.h"
#include "decoration/overview_window_frame.h"
#include "decoration/window_visual_style.h"
#include "display/callback_native_display_delegate.h"
#include "display/display_configuration_controller.h"
#include "display/display_preferences.h"
#include "display/display_topology_coordinator.h"
#include "input/cursor_shape.h"
#include "input/drag.h"
#include "ipc/display_config_service.h"
#include "render/egl_renderer.h"
#include "render/renderer.h"
#include "scene/desk_switch_node.h"
#include "scene/overview_node.h"
#include "scene/slide_out_node.h"
#include "session/session_state.h"
#include "switcher/switcher_view.h"
#include "wayland/idle_inhibition.h"
#include "wayland/input_method_launcher.h"
#include "wayland/privileged_client.h"
#include "wayland/protocol_globals.h"
#include "wayland/protocols/image_capture.h"
#include "wayland/protocols/input_method.h"
#include "wayland/protocols/dmabuf_feedback_manager.h"
#include "wayland/protocols/dmabuf_scanout_policy.h"
#include "wayland/protocols/linux_dmabuf.h"
#include "wayland/protocols/linux_drm_syncobj.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wp_presentation.h"
#include "wayland/security_delegate.h"
#include "wayland/server.h"
#include "wayland/shell_control_channel.h"
#include "wm/demo_controller.h"
#include "wm/desks/desk_preferences.h"
#include "wm/desks/desks_controller.h"
#include "wm/gestures/wm_gesture_handler.h"
#include "wm/mru_window_tracker.h"
#include "wm/overview/overview_controller.h"
#include "wm/quit_all.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_cycle_controller.h"

#ifdef CUTEFISH_ENABLE_XWAYLAND
#include "xwayland/xwayland_server.h"
#endif

namespace cutefish::core {

namespace {
// After the login transition and the shell's own startup.
constexpr int kSwitcherPreloadDelayMs = 3000;

// Whether `node` puts anything on the output, walked as the damage tracker does.
bool drawsAnything(const scene::Node &node)
{
    if (!node.isVisible() && !node.paintsWhenHidden())
        return false;
    if (node.content() != scene::Node::Content::None)
        return true;
    return std::any_of(node.children().begin(), node.children().end(),
                       [](const scene::Node *child) {
                           return drawsAnything(*child);
                       });
}
} // namespace

void CompositorShell::SecureScanoutPolicy::setRuntime(display::DisplayRuntime *runtime)
{
    m_runtime = runtime;
    if (m_runtime)
        m_runtime->setSecureScanout(m_blocked);
}

void CompositorShell::SecureScanoutPolicy::onSessionStateChanged(session::SessionState state)
{
    m_blocked = session::isSessionBlocked(state);
    if (!m_runtime)
        return;
    m_runtime->setSecureScanout(m_blocked);
    // Unlocking stays secure, so a frame the lock drew could otherwise end it.
    if (state == session::SessionState::Locking || state == session::SessionState::Unlocking)
        m_runtime->withdrawSecurePresentation();
}

void CompositorShell::IdleInhibitionPolicy::setInhibition(wayland::IdleInhibition *inhibition)
{
    m_inhibition = inhibition;
    if (m_inhibition)
        m_inhibition->setIgnored(m_blocked);
}

void CompositorShell::IdleInhibitionPolicy::onSessionStateChanged(session::SessionState state)
{
    m_blocked = session::isSessionBlocked(state);
    if (m_inhibition)
        m_inhibition->setIgnored(m_blocked);
}

void CompositorShell::ComputerControlPolicy::setController(
        computer_control::Controller *controller)
{
    m_controller = controller;
}

void CompositorShell::ComputerControlPolicy::onSessionStateChanged(session::SessionState state)
{
    if (m_controller && session::isSessionBlocked(state))
        m_controller->sessionBlocked();
}

CompositorShell::CompositorShell(const CompositorOptions &options)
    : m_options(options)
    , m_surfaceContainer(
              m_scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces"))))
    , m_inputMethodContainer(m_scene.root()->addChild(
              std::make_unique<scene::Node>(QStringLiteral("input-method"))))
    , m_switcherContainer(m_scene.root()->addChild(
              std::make_unique<scene::Node>(QStringLiteral("switcher"))))
    , m_screenTransition(static_cast<scene::ScreenTransitionNode *>(
              m_scene.root()->addChild(std::make_unique<scene::ScreenTransitionNode>())))
    , m_secureContainer(
              m_scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("secure"))))
    , m_startupCover(static_cast<scene::ScreenTransitionNode *>(
              m_scene.root()->addChild(std::make_unique<scene::ScreenTransitionNode>())))
    , m_cursorContainer(
              m_scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("cursor"))))
    , m_compositor(m_surfaceContainer)
    , m_seat(&m_compositor)
    , m_dragIconContainer(m_cursorContainer->addChild(
              std::make_unique<scene::Node>(QStringLiteral("drag-icons"))))
    , m_clientCursorContainer(m_cursorContainer->addChild(
              std::make_unique<scene::Node>(QStringLiteral("client-cursors"))))
    , m_cursor(m_seat, *m_cursorContainer)
    , m_cursorPreferences(m_cursor)
    , m_inputManager(&m_seat)
    , m_windowManager(&m_compositor, &m_seat)
    , m_themePreferences(m_decorationTheme)
    , m_decorationProvider(&m_decorationTheme)
    , m_shadowProvider(&m_decorationTheme)
    , m_shadowController(&m_windowManager, &m_shadowProvider, &m_shadowDelegate)
    , m_dockLayout(&m_windowManager)
    , m_dockPreferences(m_dockLayout, m_windowManager)
    , m_secureScene(m_secureContainer)
    , m_sessionTeardown(&m_seat, &m_windowManager)
    // The session says which screen the compositor starts on; the compositor
    // never works it out from anything about a user.
    , m_sessionController(&m_secureScene, &m_sessionTeardown,
                          options.login ? session::SessionState::Login
                                        : session::SessionState::Active)
    , m_authUi(&m_sessionController, m_seat.xkbTracker())
    , m_screenSaver(&m_sessionController)
    , m_sleepLock(&m_sessionController)
    , m_compositorInstanceId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    m_compositor.setOutputScale(m_options.scale);
    m_compositor.setInputMethodContainer(m_inputMethodContainer);
    m_seat.setDragIconContainer(m_dragIconContainer);
    m_seat.setCursorContainer(m_clientCursorContainer);
    m_windowManager.setDisplayManager(&m_displayManager);
    m_windowManager.setFrameProvider(&m_decorationProvider);
    m_windowManager.setVisualStyle(m_decorationTheme.visualStyle());
    m_decorationTheme.addObserver(this);
    QObject::connect(&m_compositor, &compositor::Compositor::frameStarted, &m_dockLayout,
                     &wm::DockLayoutManager::advance);
    m_outputModel.scale = m_options.scale;

    // The overview's windows slide with a desk switch, its desk bar stays.
    m_overviewNode = static_cast<scene::OverviewNode *>(
            m_surfaceContainer->addChild(std::make_unique<scene::OverviewNode>()));
    m_deskSwitchNode = static_cast<scene::DeskSwitchNode *>(
            m_surfaceContainer->addChild(std::make_unique<scene::DeskSwitchNode>()));
    m_windowMoveNode = static_cast<scene::SlideOutNode *>(
            m_surfaceContainer->addChild(std::make_unique<scene::SlideOutNode>()));
    m_overviewBarNode = static_cast<scene::OverviewDeskBarNode *>(m_surfaceContainer->addChild(
            std::make_unique<scene::OverviewDeskBarNode>(m_overviewNode)));
    m_windowManager.setDeskOverlays(
            {m_overviewNode, m_deskSwitchNode, m_windowMoveNode, m_overviewBarNode});
    const auto paintText = [this](const QString &text, int pixelSize, const render::Color &color,
                                  double density, int maxWidth) {
        decoration::TextRenderer &renderer = m_decorationTheme.textRenderer();
        const decoration::Image image = renderer.renderText(
                renderer.elideText(text, pixelSize, maxWidth), pixelSize, color, density);
        if (image.size().isEmpty())
            return QImage();
        return QImage(image.bits(), image.size().width(), image.size().height(), image.stride(),
                      QImage::Format_RGBA8888_Premultiplied)
                .copy();
    };
    m_overviewNode->setTextPainter(paintText);
    m_overviewBarNode->setTextPainter(paintText);
    m_overviewNode->setWindowFrame(decoration::overviewWindowFrame(&m_decorationTheme));
    wm::DesksController *desks = m_windowManager.desks();
    desks->setSwitchNode(m_deskSwitchNode);
    desks->setWindowMoveNode(m_windowMoveNode);
    desks->setScreenshotter([this](const std::function<void(render::Renderer &)> &paint) {
        return m_displayRuntime ? m_displayRuntime->captureOutputs(paint)
                                : scene::DeskSwitchNode::Frames();
    });
}

void CompositorShell::initKeyboard()
{
    const auto applyRepeat = [this] {
        input::KeyRepeatSettings settings;
        settings.enabled = m_keyboardSettings.repeatEnabled();
        settings.delay = std::chrono::milliseconds(m_keyboardSettings.repeatDelay());
        settings.interval = std::chrono::milliseconds(m_keyboardSettings.repeatInterval());
        m_seat.setKeyRepeatSettings(settings);
        m_authUi.setKeyRepeatSettings(settings);
        if (m_acceleratorFilter)
            m_acceleratorFilter->setKeyRepeatSettings(settings);
    };
    applyRepeat();
    QObject::connect(&m_keyboardSettings, &input::KeyboardSettings::repeatEnabledChanged, qApp,
                     applyRepeat);
    QObject::connect(&m_keyboardSettings, &input::KeyboardSettings::repeatDelayChanged, qApp,
                     applyRepeat);
    QObject::connect(&m_keyboardSettings, &input::KeyboardSettings::repeatIntervalChanged, qApp,
                     applyRepeat);

    m_keyboardSettings.setLayoutApplier([this](const QString &name) {
        return m_seat.setKeyboardLayout(name.toStdString());
    });

    // The greeter and development instances must not take over the user's
    // keyboard settings.
    if (!m_options.noDrm && !m_options.login)
        m_keyboardSettings.registerService();
}

void CompositorShell::initComputerControl()
{
    // As with capture: the greeter has no one to hand the seat to, and a
    // development instance has no display to show.
    if (m_options.login || m_options.noDrm)
        return;

    const auto showsSystemDialogs = [this](qint64 pid) {
        return m_security && m_security->showsSystemDialogs(pid);
    };
    m_computerControlConsent =
            std::make_unique<computer_control::PermissionConsent>(showsSystemDialogs);

    ComputerControlHosts hosts;
    hosts.windowManager = &m_windowManager;
    hosts.seat = &m_seat;
    hosts.session = &m_sessionController;
    hosts.consent = m_computerControlConsent.get();
    hosts.inputMethod = [this]() -> wayland::InputMethodManager * {
        return m_globals ? m_globals->inputMethod() : nullptr;
    };
    hosts.showsSystemDialogs = showsSystemDialogs;
    m_computerControl = std::make_unique<computer_control::Controller>(
            &m_inputManager, computerControlEnvironment(std::move(hosts)));
    m_computerControlPolicy.setController(m_computerControl.get());
    QObject::connect(&m_compositor, &compositor::Compositor::outputDrawn,
                     m_computerControl.get(), &computer_control::Controller::drawn);

    m_computerControlService =
            std::make_unique<computer_control::ComputerControlService>(m_computerControl.get());
    if (!m_computerControlService->registerService())
        qCWarning(cfWayland) << "The computer control service is unavailable";
}

void CompositorShell::initAccelerators()
{
    m_windowCycle = std::make_unique<wm::WindowCycleController>(
            &m_windowManager, m_windowManager.mruWindowTracker());
    m_switcherView = std::make_unique<switcher::SwitcherView>(m_switcherContainer,
                                                              &m_windowManager,
                                                              &m_decorationTheme);
    m_windowCycle->setView(m_switcherView.get());
    // Once the session has settled; the greeter switches nothing.
    if (!m_options.login) {
        QTimer::singleShot(kSwitcherPreloadDelayMs, m_switcherView.get(),
                           [view = m_switcherView.get()] {
                               view->preload();
                           });
    }
    m_acceleratorConfiguration = std::make_unique<accelerators::AcceleratorConfiguration>();
    m_acceleratorConfiguration->initialize();
    m_acceleratorDelegate = std::make_unique<accelerators::DBusAcceleratorDelegate>();

    accelerators::CommandContext context;
    context.windowManager = &m_windowManager;
    context.windowCycle = m_windowCycle.get();
    context.session = &m_sessionController;
    context.screenSaver = &m_screenSaver;
    context.delegate = m_acceleratorDelegate.get();
    m_acceleratorController = std::make_unique<accelerators::AcceleratorController>(
            m_acceleratorConfiguration.get(), context, [this] {
                return !m_seat.pressedKeys().empty();
            });
    m_acceleratorController->setUsePositionalLookup(
            accelerators::usesPositionalShortcuts(*m_seat.xkbTracker()));

    // The greeter has no overview. It takes the pointer and the keys from
    // the windows while it is up, after the accelerators had them.
    if (!m_options.login) {
        wm::OverviewController *overview = m_windowManager.overview();
        overview->setNodes(m_overviewNode, m_overviewBarNode);
        overview->setNext(m_inputManager.eventSink());
        m_inputManager.setEventSink(overview);
    }

    // In front of whatever the window manager chain is: the demo controller
    // when enabled, the window manager otherwise.
    m_acceleratorFilter = std::make_unique<accelerators::AcceleratorFilter>(
            m_acceleratorController.get(), m_seat.xkbTracker(), &m_windowManager,
            m_windowCycle.get(), m_inputManager.eventSink());
    m_acceleratorFilter->setKeyRepeatSettings(m_seat.keyRepeatSettings());
    m_inputManager.setEventSink(m_acceleratorFilter.get());

    m_acceleratorService = std::make_unique<accelerators::AcceleratorService>(
            m_acceleratorConfiguration.get(), m_acceleratorController.get(),
            m_seat.xkbTracker());
    m_acceleratorService->registerService();

    // Quit's last resort for a process that closed its windows but keeps
    // running. AppRuntime checks the process is the user's before signalling.
    m_windowManager.setTerminateProcess([](qint64 pid) {
        QDBusMessage message = QDBusMessage::createMethodCall(
                QStringLiteral("com.cutefish.AppRuntime"), QStringLiteral("/AppRuntime"),
                QStringLiteral("com.cutefish.AppRuntime"), QStringLiteral("quitByPid"));
        message << static_cast<uint>(pid);
        QDBusConnection::sessionBus().asyncCall(message);
    });
    // Force quit ends the client itself; AppRuntime ends what else runs in the
    // application scope it started the process in.
    m_windowManager.setForceQuitProcess([](qint64 pid) {
        QDBusMessage message = QDBusMessage::createMethodCall(
                QStringLiteral("com.cutefish.AppRuntime"), QStringLiteral("/AppRuntime"),
                QStringLiteral("com.cutefish.AppRuntime"), QStringLiteral("forceQuitByPid"));
        message << static_cast<uint>(pid);
        QDBusConnection::sessionBus().asyncCall(message);
    });

    input::KeyTranslator *translator = m_seat.keyTranslator();
    translator->setTerminalProvider([this] {
        compositor::Surface *focus = m_seat.focusedSurface();
        const wm::Window *window = focus ? m_windowManager.windowForSurface(focus) : nullptr;
        return window && input::isTerminal(window->applicationId());
    });
    // A remote desktop, a virtual machine or the shortcut recorder wants the
    // keys the user pressed, with Command as Meta. So does a game, which holds
    // the pointer and reads a lone Ctrl or Alt as it goes down.
    translator->setPassthroughProvider([this] {
        return m_seat.shortcutsInhibition()->isActive()
                || m_seat.pointerConstraints()->active()
                || m_acceleratorController->shouldPreventProcessingAccelerators();
    });

    // The convention decides the compositor's own shortcuts as well as what
    // the clients get for the keys it leaves them.
    const auto applyConvention = [this] {
        const input::ShortcutConvention convention = m_keyboardSettings.shortcutConvention();
        m_seat.setShortcutConvention(convention);
        m_acceleratorConfiguration->setConvention(convention);
    };
    applyConvention();
    QObject::connect(&m_keyboardSettings, &input::KeyboardSettings::shortcutConventionChanged,
                     qApp, applyConvention);
}

void CompositorShell::onThemeChanged()
{
    m_windowManager.setVisualStyle(m_decorationTheme.visualStyle());
    // Shadows and silhouettes are shaped at paint time, so the whole scene repaints.
    m_scene.root()->notifyChanged();
}

bool CompositorShell::init()
{
    m_sessionTeardown.setCursorReset([this] {
        m_cursor.refresh();
    });
    m_sessionController.addObserver(&m_secureScanoutPolicy);
    m_sessionController.addObserver(&m_idleInhibitionPolicy);
    m_sessionController.addObserver(&m_computerControlPolicy);
    QObject::connect(&m_authUi, &auth::AuthUi::cursorShapeChanged, qApp,
                     [this](Qt::CursorShape shape) {
                         m_seat.setCompositorCursorShape(input::cursorShapeFromQt(shape));
                     });
    m_authUi.setPointerLocator([this] {
        return m_seat.cursorPosition();
    });
    m_screenSaver.registerService();
    // The greeter and development instances must not hold the system's sleep.
    if (!m_options.noDrm && !m_options.login)
        m_sleepLock.start();
    m_seat.setSessionGate(&m_sessionController);
    m_windowManager.setSessionGate(&m_sessionController);
    QObject::connect(
            &m_displayManager, &display::DisplayManager::displaysChanged, &m_compositor, [this] {
                if (m_displayManager.applied())
                    m_sessionController.setDisplayConfiguration(*m_displayManager.applied());
                else
                    m_sessionController.setOutputs({});
            });

    m_inputManager.setEventSink(&m_windowManager);
    QObject::connect(&m_windowManager, &wm::WindowManager::pointerTargetsChanged,
                     &m_inputManager, &input::InputManager::synthesizeMotion);
    if (m_options.demoEnabled()) {
        m_demoController = std::make_unique<wm::DemoController>(&m_windowManager, &m_inputManager);
        m_inputManager.setEventSink(m_demoController.get());
    }
    // Before the accelerators, which read the layout once to pick the lookup.
    initKeyboard();
    // Before Wayland, whose control global and capture filter read it.
    initComputerControl();
    initAccelerators();

    // The greeter has no desks, and development instances must not take over
    // the user's.
    if (!m_options.login) {
        m_gestureHandler = std::make_unique<wm::WmGestureHandler>(
                m_windowManager.desks(),
                [this] {
                    return m_seat.drag()->busy() || m_windowManager.eventHandler()->isDragging();
                },
                m_windowManager.overview());
        // After idle the overview's first frame missed its slot on every other
        // swipe; with one drawn ahead, none did. That one may land late unseen.
        m_gestureHandler->setWarmUp([this] {
            for (const auto &output : m_backend->allOutputs())
                output->warmUp();
        });
        m_inputManager.setGestureFilter(m_gestureHandler.get());
    }
    if (!m_options.noDrm && !m_options.login)
        m_deskPreferences = std::make_unique<wm::DeskPreferences>(m_windowManager.desks());

    m_powerButton = std::make_unique<session::PowerButton>(&m_sessionController,
                                                           m_inputManager.eventSink());
    m_inputManager.setEventSink(m_powerButton.get());
    // Elsewhere logind keeps the key and powers the machine off.
    if (!m_options.noDrm && !m_options.login)
        m_powerButton->start();

    // The greeter has no applications of its own to quit.
    if (!m_options.login) {
        m_quitAll = std::make_unique<wm::QuitAll>(&m_windowManager);
        m_applicationsService = std::make_unique<session::ApplicationsService>(
                m_quitAll.get(), [this](bool ending) {
                    m_dockLayout.setSessionEnding(ending);
                });
        if (!m_applicationsService->registerService())
            qCWarning(cfWayland) << "The applications service is unavailable";

        m_windowDebugService = std::make_unique<session::WindowDebugService>(&m_windowManager);
        if (m_options.noDrm)
            m_windowDebugService->setInput(&m_inputManager);
        m_windowDebugService->setOutputStats([this] {
            QStringList lines;
            if (m_backend) {
                for (const auto &output : m_backend->allOutputs())
                    lines << output->statsLine();
            }
            return lines;
        });
        if (!m_windowDebugService->registerService())
            qCWarning(cfWayland) << "The window debug service is unavailable";
    }

    if (m_options.noDrm) {
        // Headless: give the window manager a nominal output so state changes
        // and placement still have somewhere to happen.
        m_windowManager.setOutputBounds(
                QRect(QPoint(0, 0), backend::HeadlessBackend::outputSize()));
        m_scene.setSize(backend::HeadlessBackend::outputSize());
        m_sessionController.setOutputs(
                {{QStringLiteral("headless"),
                  QRect(QPoint(0, 0), backend::HeadlessBackend::outputSize())}});
    }

    if (m_options.wmDemoScript)
        m_demoController->startScript();

    if (!initWayland())
        return false;

    initXwayland();

    // A shell that goes away leaves the compositor and every application it was
    // showing untouched; only its own reservations have to be released, since
    // its windows disappear with its surfaces.
    m_globals->privileged()->setShellDisconnectedCallback([this] {
        m_windowManager.clearAllExclusiveZones();
    });

    if (!m_options.shellControlSocket.isEmpty()) {
        m_shellControl = std::make_unique<wayland::ShellControlChannel>();
        QObject::connect(m_shellControl.get(), &wayland::ShellControlChannel::connectionReceived,
                         qApp, [this](int fd) {
                             m_globals->privileged()->adoptShellConnection(fd);
                         });
        if (!m_shellControl->listen(m_options.shellControlSocket))
            qCWarning(cfWayland) << "Continuing without a trusted shell channel";
    }

    m_sceneSync.setSingleShot(true);
    m_sceneSync.setInterval(0);
    QObject::connect(&m_sceneSync, &QTimer::timeout, qApp, [this] { syncSceneToClients(); });
    QObject::connect(&m_compositor, &compositor::Compositor::repaintNeeded, qApp,
                     [this] { scheduleSceneSync(); });
    m_scene.root()->setChangeCallback([this] { scheduleSceneSync(); });

    if (m_options.noDrm) {
        if (!initHeadless())
            return false;
    } else {
        // Its layout starts at scale 1 and passes an empty one before the real
        // one: nothing of this compositor's goes on the cursor plane until then.
        m_cursorHeld = true;
        m_cursor.setHeld(true);
        if (!initDrm())
            return false;
        // After initDrm(), which can wait long for the seat; the displays settle only after it.
        m_cursorRelease.setSingleShot(true);
        m_cursorRelease.callOnTimeout([this] {
            releaseCursor();
        });
        m_cursorRelease.start(2000);
    }

    return initDisplay();
}

bool CompositorShell::initWayland()
{
    m_server = std::make_unique<wayland::Server>();
    if (!m_server->display())
        return false;

    m_security = std::make_unique<wayland::ExecutableSecurityDelegate>(
            QStringList {QStringLiteral("/usr/bin/cutefish-services")} + m_options.clipboardClients,
            QStringList {QCoreApplication::applicationDirPath() + "/cutefish-screenshot",
                         QStringLiteral(CUTEFISH_PORTAL_BACKEND),
                         QStringLiteral(CUTEFISH_SCREEN_RECORDER)}
                    + m_options.captureClients,
            // Permission prompts and authentication dialogs.
            QStringList {QStringLiteral("/usr/bin/cutefish-services"),
                         QStringLiteral("/usr/bin/cutefish-polkit-agent")},
            !m_options.noDrm,
            QStringList {QStringLiteral("/usr/bin/cutefish-computer-use")}
                    + m_options.computerControlClients);
    m_windowManager.setSystemDialogPolicy([this](qint64 pid) {
        return m_security->showsSystemDialogs(pid);
    });

    wayland::ProtocolGlobals::Dependencies dependencies;
    dependencies.server = m_server.get();
    dependencies.compositor = &m_compositor;
    dependencies.seat = &m_seat;
    dependencies.inputManager = &m_inputManager;
    dependencies.windowManager = &m_windowManager;
    dependencies.shadowController = &m_shadowController;
    dependencies.inputMethodPopupLayer = m_inputMethodContainer;
    dependencies.security = m_security.get();
    dependencies.computerControl = m_computerControl.get();
    // The greeter and development instances must not take over the user's input
    // method.
    dependencies.launchInputMethod =
            !m_options.inputMethodId.isEmpty() || (!m_options.noDrm && !m_options.login);
    dependencies.inputMethodOverride = m_options.inputMethodId;
    // Constructing this installs the privileged registry filter, which has to be
    // in place before the socket exists.
    m_globals = std::make_unique<wayland::ProtocolGlobals>(dependencies);

    if (!m_server->addSocket(m_options.socketName))
        return false;

    if (!m_server->start())
        return false;

    if (!m_globals->initialize())
        return false;
    m_idleInhibitionPolicy.setInhibition(m_globals->idleInhibition());

    // The greeter and development instances must not keep the user's session
    // awake. The counter goes before the service when the shell tears down.
    if (!m_options.noDrm && !m_options.login) {
        const QPointer<wayland::IdleInhibition> inhibition = m_globals->idleInhibition();
        m_screenSaver.setInhibition(
                [inhibition] {
                    if (inhibition)
                        inhibition->acquire();
                },
                [inhibition] {
                    if (inhibition)
                        inhibition->release();
                });
    }

    auto *imageCapture = m_globals->imageCapture();
    imageCapture->setCaptureAllowed([this] {
        return m_sessionController.allowsCapture();
    });
    m_seat.addObserver(this);
    m_sessionTeardown.setCaptureAbort([imageCapture] {
        imageCapture->failPending();
    });
    m_sessionTeardown.setWindowCycle(m_windowCycle.get());
    QObject::connect(&m_compositor, &compositor::Compositor::outputDrawn, imageCapture,
                     [imageCapture](uint64_t generation, uint64_t aimedNs) {
                         imageCapture->drawn(generation, aimedNs);
                     });
    QObject::connect(&m_compositor, &compositor::Compositor::outputPresented, imageCapture,
                     [imageCapture](uint64_t generation, uint64_t timeNs) {
                         imageCapture->presented(generation, timeNs);
                     });
    QObject::connect(&m_compositor, &compositor::Compositor::outputDiscarded, imageCapture,
                     [imageCapture](uint64_t generation) {
                         imageCapture->discarded(generation);
                     });
    QObject::connect(&m_compositor, &compositor::Compositor::outputRendered, imageCapture,
                     [this, imageCapture](render::Renderer &renderer) {
                         wayland::ImageCaptureManager::Painters painters;
                         painters.root = m_scene.root();
                         painters.cursors = [this, &renderer] {
                             m_clientCursorContainer->render(renderer);
                             m_cursor.paintForCapture(renderer);
                         };
                         painters.scene = [this, &renderer, &painters](bool cursors) {
                             renderer.clear(m_scene.background());
                             // The capture interface stays out of what it captures.
                             m_surfaceContainer->renderForCapture(renderer);
                             m_inputMethodContainer->render(renderer);
                             m_switcherContainer->render(renderer);
                             m_dragIconContainer->render(renderer);
                             if (cursors)
                                 painters.cursors();
                         };
                         painters.cursorBounds = [this] {
                             return m_cursor.captureBounds().toAlignedRect();
                         };
                         painters.composited = [this] {
                             using Composited = wayland::ImageCaptureManager::Composited;
                             // On the output, but never in a capture.
                             if (drawsAnything(*m_screenTransition)
                                 || drawsAnything(*m_secureContainer)
                                 || drawsAnything(*m_startupCover))
                                 return Composited::Unusable;
                             for (const scene::Node *child : m_surfaceContainer->children()) {
                                 if (child->isExcludedFromCapture() && drawsAnything(*child))
                                     return Composited::Unusable;
                             }
                             // A software or client cursor; a hardware one is on its own plane.
                             const auto &cursors = m_cursorContainer->children();
                             const bool cursor = std::any_of(
                                     cursors.begin(), cursors.end(),
                                     [this](const scene::Node *child) {
                                         return child != m_dragIconContainer
                                                 && drawsAnything(*child);
                                     });
                             return cursor ? Composited::WithCursor : Composited::WithoutCursor;
                         };
                         imageCapture->renderPending(renderer, painters);
                     });

    return true;
}

void CompositorShell::initXwayland()
{
#ifdef CUTEFISH_ENABLE_XWAYLAND
    m_xwayland = std::make_unique<xwayland::XwaylandServer>(
            m_server->display(), m_globals->privileged(), &m_windowManager);
    // Xwayland starts on demand, and an input method that connected to its
    // display would start it at login. It runs without X11 until Xwayland
    // does, then restarts with the display to serve X11 clients as well.
    m_xwayland->ready = [this] {
        m_globals->startInputMethod({}, {});
    };
    m_xwayland->started = [this] {
        m_globals->setInputMethodX11Display(m_xwayland->display(), m_xwayland->authority());
    };
    m_xwayland->failure = [this](const QString &) {
        m_globals->startInputMethod({}, {});
    };
    if (m_options.xwayland) {
        // The saved display configuration may already be applied, with a
        // scale of its own per display: render at the largest.
        double scale = 0;
        if (const auto &applied = m_displayManager.applied()) {
            for (const auto &logical : applied->logicalDisplays)
                scale = std::max(scale, logical.scale.value());
        }
        if (scale <= 0)
            scale = m_compositor.outputScale().value();
        m_xwayland->setScale(scale);
        xwayland::XwaylandServer::Options options;
        options.software = m_options.xwaylandSoftware;
        options.readinessFile = m_options.xwaylandReadyFile;
        options.lazy = true;
        m_xwayland->start(options);
    } else {
        m_globals->startInputMethod({}, {});
    }
#else
    m_globals->startInputMethod({}, {});
#endif
}

QList<compositor::Output *> CompositorShell::outputModels() const
{
    QList<compositor::Output *> result;
    if (m_backend && m_backend->allOutputs().empty())
        return result;
    result.append(const_cast<compositor::Output *>(&m_outputModel));
    for (const auto &output : m_additionalOutputModels)
        result.append(output.get());
    return result;
}

void CompositorShell::publishDisplayState()
{
    display::DisplayRuntime::initialize(m_displayManager, m_backend.get(), outputModels(),
                                        m_compositorInstanceId, m_compositor, m_inputManager);
    if (!std::exchange(m_cursorProbed, true))
        probeCursor();
}

// The first probe sees the cursor the previous DRM master left, before this
// compositor has touched the plane.
void CompositorShell::probeCursor()
{
    if (!m_backend)
        return;
    const auto models = outputModels();
    const auto outputs = m_backend->allOutputs();
    for (qsizetype index = 0; index < models.size() && index < outputs.size(); ++index) {
        if (const auto topLeft = outputs.at(index)->probedCursor()) {
            m_probedCursor = ProbedCursor {display::DisplayId(models.at(index)->displayId), *topLeft};
            return;
        }
    }
}

// Once the displays have their real scale, the pointer takes the previous
// master's cursor over in place and this cursor replaces it once, at its size;
// with none, as after boot, the pointer stays centred.
void CompositorShell::releaseCursor()
{
    if (!std::exchange(m_cursorHeld, false))
        return;
    m_cursorRelease.stop();
    const auto mapping = m_probedCursor ? m_displayManager.mappingFor(m_probedCursor->destination)
                                        : std::nullopt;
    if (mapping) {
        // In native pixels, with the hotspot the plane subtracts again: a logical
        // hotspot is rounded and would move the image a pixel or two.
        const QPoint native = m_probedCursor->topLeft + m_cursor.deviceHotspot();
        const QRect content = mapping->nativeContent;
        const QPointF pointer(std::clamp(native.x(), content.left(), content.right()),
                              std::clamp(native.y(), content.top(), content.bottom()));
        if (const auto position = mapping->transform().nativeToLogical(pointer))
            m_inputManager.warpPointer(*position);
    }
    m_cursor.setHeld(false);
}

void CompositorShell::applyScale()
{
    m_updatingOutput = true;
    int x = 0;
    QRect desktop;
    for (auto *model : outputModels()) {
        model->scale = m_compositor.outputScale();
        model->renderScale = model->scale.value();
        model->nativeContentRect = QRect(QPoint(), model->pixelSize);
        model->position = QPoint(x, 0);
        desktop = desktop.united(QRect(model->position, model->logicalSize()));
        x += model->logicalSize().width();
    }
    m_scene.setSize(desktop.size());
    m_windowManager.setOutputBounds(desktop);
    m_inputManager.setDeviceScale(m_outputModel.scale.value());
    m_inputManager.setDeviceBounds(m_outputModel.pixelSize);
    m_cursor.setScale(m_outputModel.scale.value());
    if (m_backend && m_backend->renderer())
        m_backend->renderer()->setRasterScale(m_outputModel.scale.value());
#ifdef CUTEFISH_ENABLE_XWAYLAND
    // Before the outputs are re-sent: Xwayland sizes its screen from them.
    if (m_xwayland)
        m_xwayland->setScale(m_outputModel.scale.value());
#endif
    const auto models = outputModels();
    for (int index = 0; index < models.size() && index < int(m_outputGlobals.size()); ++index)
        wayland::updateOutputGlobal(m_outputGlobals[index], models.at(index));
    publishDisplayState();
    m_updatingOutput = false;
    for (auto *global : m_outputGlobals)
        wayland::updateOutputMembership(global);
}

void CompositorShell::scheduleSceneSync()
{
    if (!m_sceneSync.isActive())
        m_sceneSync.start();
}

void CompositorShell::syncSceneToClients()
{
    // A surface can only have entered or left an output, or come to prefer
    // another scale, if it moved, as exo updates a surface's display only
    // when its window changes displays (SurfaceTreeHost::UpdateDisplayOnTree).
    const auto moved = m_compositor.takeMovedSurfaces();
    for (const auto &token : moved) {
        if (*token)
            (*token)->notifyOutputScaleChanged();
    }
    if (!m_updatingOutput && !moved.empty()) {
        for (auto *global : m_outputGlobals)
            wayland::updateOutputMembership(global, moved);
    }
    if (m_globals)
        m_globals->sceneChanged();
}

bool CompositorShell::initHeadless()
{
    m_headless =
            std::make_unique<backend::HeadlessBackend>(&m_compositor, m_options.headlessOutputs);
    QObject::connect(m_headless.get(), &backend::HeadlessBackend::aboutToPresent, &m_compositor,
                     [this] {
                         m_sessionController.notifyFramePresented();
                     });
    m_additionalOutputModels = m_headless->createOutputs(&m_outputModel);
    m_headless->startClock();

    for (auto *model : outputModels()) {
        auto *global = wayland::createOutputGlobal(m_server->display(), model);
        if (!global)
            return false;
        m_outputGlobals.push_back(global);
    }
    m_outputGlobal = m_outputGlobals.front();
    applyScale();
    return true;
}

bool CompositorShell::initDrm()
{
    m_session = backend::LibseatSession::create();
    if (!m_session)
        return false;

    m_backend = std::make_unique<backend::DrmBackend>(m_session.get());
    if (!m_backend->open(m_options.drmDevice))
        return false;
    if (!m_backend->createOutputs())
        return false;
    if (!m_backend->initRenderer() || !m_backend->refreshDevices())
        return false;

    // linux-dmabuf can only be advertised once the renderer exists: what it
    // advertises is exactly what this EGL/GBM stack can import.
    m_compositor.setDmaBufDevice(m_backend->renderer());
    m_globals->imageCapture()->setRenderer(m_backend->renderer());
    m_dmabufGlobal = wayland::LinuxDmabufGlobal::create(m_server->display(), &m_compositor,
                                                        m_backend->renderer());
    if (!m_dmabufGlobal) {
        qCWarning(cfWayland) << "linux-dmabuf is unavailable on this device";
    } else {
        // A fullscreen client is steered to a layout the primary plane takes,
        // so its buffers can be flipped instead of composited.
        auto *feedback = m_dmabufGlobal->feedbackManager();
        feedback->setScanoutFormats([this]() -> wayland::ScanoutFormats {
            if (auto *output = m_backend->primaryOutput())
                return output->primaryPlaneFormats();
            return {};
        });
        m_scanoutPolicy = std::make_unique<wayland::DmaBufScanoutPolicy>(feedback, &m_windowManager);
        QObject::connect(&m_displayManager, &display::DisplayManager::displaysChanged,
                         m_scanoutPolicy.get(), [feedback] { feedback->refreshScanoutTranches(); });
    }

    // Explicit synchronization rides on the same device the buffers come from.
    // Without it clients fall back to implicit fences, which is slower but
    // correct.
    m_syncobjGlobal = wayland::createLinuxDrmSyncobjGlobal(m_server->display(), m_backend->fd());
    if (!m_syncobjGlobal)
        qCWarning(cfWayland) << "Explicit synchronization is unavailable on this device";

    const auto populateOutputModel = [](auto *output, auto *model) {
        model->pixelSize = output->size();
        model->physicalSizeMm = output->physicalSizeMm();
        model->name = output->publicName();
        model->model = output->name();
        model->refreshMilliHz = qRound(output->refreshRate() * 1000);
    };
    if (auto *output = m_backend->primaryOutput())
        populateOutputModel(output, &m_outputModel);
    for (size_t index = 1; index < m_backend->allOutputs().size(); ++index) {
        auto model = std::make_unique<compositor::Output>();
        model->scale = m_options.scale;
        populateOutputModel(m_backend->allOutputs()[index], model.get());
        m_additionalOutputModels.push_back(std::move(model));
    }
    m_outputGlobals.resize(m_backend->allOutputs().size(), nullptr);
    applyScale();
    m_cursorRouter = std::make_unique<cursor::OutputCursorRouter>();
    if (m_backend->hasMonotonicPresentationClock())
        wayland::createPresentationGlobal(m_server->display());
    m_scene.root()->setChangeCallback([this] {
        scheduleSceneSync();
        for (const auto &output : m_backend->allOutputs())
            output->scheduleFrame(backend::DrmOutput::Reason::Scene);
    });

    m_inputBackend = backend::LibinputBackend::create(m_session.get(), &m_inputManager);
    if (!m_inputBackend)
        qCWarning(cfInput) << "Continuing without input devices";

    QObject::connect(m_session.get(), &backend::Session::activeChanged, qApp, [this](bool active) {
        qInfo() << "Compositor seat active" << active;
        for (const auto &output : m_backend->allOutputs())
            output->reportStats();
        if (!active) {
            m_seat.drag()->cancel();
            m_scene.retireClosing();
        }
        if (m_inputBackend)
            m_inputBackend->setActive(active);
    });
    return true;
}

bool CompositorShell::initDisplay()
{
    if (!m_displayManager.hardware() || !m_displayManager.applied())
        return true;

    m_displayRuntime = std::make_unique<display::DisplayRuntime>(
            m_displayManager, m_backend.get(), outputModels(), m_outputGlobals, m_outputGlobal,
            m_server->display(), m_compositor, m_inputManager, m_windowManager, m_scene, m_cursor,
            m_cursorRouter.get(), m_updatingOutput);
    // The login screen is ready once its wallpaper and panel are drawn, the
    // session once its desktop covers the screen.
    m_displayRuntime->carryOverWhileSecure = m_options.login;
    m_displayRuntime->setStartupCover(
            m_startupCover,
            {m_surfaceContainer, m_inputMethodContainer, m_switcherContainer, m_screenTransition,
             m_secureContainer},
            m_options.login ? std::function<bool()>([this] {
                return m_secureScene.isWallpaperReady() && m_authUi.isPanelDrawn();
            })
                            : std::function<bool()>());
    if (!m_options.login && m_displayRuntime->isStartupCoverShowing()) {
        m_dockLayout.setSessionStarting(true);
        QObject::connect(&m_compositor, &compositor::Compositor::frameStarted, &m_dockLayout,
                         [this] {
                             if (m_displayRuntime && !m_displayRuntime->isStartupCoverShowing())
                                 m_dockLayout.setSessionStarting(false);
                         });
    }
#ifdef CUTEFISH_ENABLE_XWAYLAND
    m_displayRuntime->maximumScaleChanged = [this](double scale) {
        if (m_xwayland)
            m_xwayland->setScale(scale);
    };
#endif
    m_displayRuntime->lockRequested = [this] {
        m_sessionController.requestLock();
        m_secureScene.finishTransition();
    };
    m_displayRuntime->capturePending = [this](const QString &output) {
        return m_globals->imageCapture()->hasPending(output);
    };
    // Complete transitions only after secure presentation.
    m_displayRuntime->securelyPresented = [this](const QString &name) {
        m_sessionController.notifyOutputPresented(name);
    };
    m_displayRuntime->secureContentSettled = [this](double frameTime) {
        return m_secureScene.isSettled(frameTime);
    };
    m_secureScanoutPolicy.setRuntime(m_displayRuntime.get());
    m_displayRuntime->setScreenTransition(m_screenTransition,
                                          {m_surfaceContainer, m_inputMethodContainer,
                                           m_switcherContainer});

    m_displayDelegate = std::make_unique<display::CallbackNativeDisplayDelegate>(
            *m_displayManager.hardware(),
            [runtime = m_displayRuntime.get()](const auto &configuration, auto mode) {
                return runtime->configure(configuration, mode);
            },
            [runtime = m_displayRuntime.get()](const auto &displays, auto state) {
                return runtime->setPower(displays, state);
            });
    m_displayController = std::make_unique<display::DisplayConfigurationController>(
            &m_displayManager, m_displayDelegate.get());

    if (m_backend) {
        m_topologyCoordinator = std::make_unique<display::DisplayTopologyCoordinator>(
                &m_displayManager, m_displayDelegate.get(), m_displayController.get(), [this] {
                    return m_displayRuntime->reprobe();
                });
        QObject::connect(m_backend.get(), &backend::DrmBackend::outputsInvalidated,
                         m_topologyCoordinator.get(), [this] {
                             m_topologyCoordinator->invalidate();
                         });
        QObject::connect(m_topologyCoordinator.get(),
                         &display::DisplayTopologyCoordinator::reconciled, qApp,
                         [this](bool succeeded) {
                             releaseCursor();
                             if (!succeeded)
                                 qCWarning(cfWayland)
                                         << "Display topology recovery failed; waiting for "
                                            "another hardware event or seat activation";
                         });
        m_topologyCoordinator->start();
    } else {
        display::DisplayPreferences preferences;
        auto restored = preferences.restore(*m_displayManager.hardware(),
                                            m_displayManager.applied()->revision);
        if (restored)
            m_displayController->apply(*restored, QStringLiteral("internal"), 0);
    }

    m_nightLight = std::make_unique<display::NightLight>(
            [this](const render::ColorTransform &transform) {
                m_displayRuntime->setColorTransform(transform);
            });
    m_displayService = std::make_unique<display::ipc::DisplayConfigService>(
            &m_displayManager, m_displayController.get(), QDBusConnection::sessionBus(), [this] {
                return !m_session || m_session->isActive();
            });
    m_displayService->setNightLightHandler(
            [this](double strength, std::chrono::milliseconds duration) {
                m_nightLight->animateTo(strength, duration);
            });
    if (!m_displayService->registerService(m_options.displayConfigService))
        qCWarning(cfWayland) << "Native display configuration D-Bus service is unavailable";
    return true;
}

void CompositorShell::stop()
{
    if (m_server)
        m_server->stop();
}

QString CompositorShell::socketName() const
{
    return m_server ? m_server->socketName() : QString();
}

bool CompositorShell::notifyReady()
{
    if (m_options.readyFd < 0)
        return true;
    if (write(m_options.readyFd, "R", 1) != 1) {
        qCritical() << "Cannot notify session that the compositor is ready";
        return false;
    }
    close(m_options.readyFd);
    return true;
}

CompositorShell::~CompositorShell()
{
    m_decorationTheme.removeObserver(this);
    m_seat.removeObserver(this);
    // Input stops before the clients do: a pointer or keyboard must not be
    // dispatched to while its client is being torn down.
    m_inputBackend.reset();
    m_topologyCoordinator.reset();
    m_displayService.reset();
    m_nightLight.reset();
    m_displayController.reset();
    m_displayDelegate.reset();
    m_inputManager.setEventSink(nullptr);
    m_windowDebugService.reset();
    m_applicationsService.reset();
    m_quitAll.reset();
    m_powerButton.reset();
    m_acceleratorService.reset();
    m_acceleratorFilter.reset();
    m_acceleratorController.reset();
    // With input stopped, so what the agent holds is left to the seat's teardown.
    m_computerControlPolicy.setController(nullptr);
    m_computerControlService.reset();
    m_computerControl.reset();
    if (m_windowCycle)
        m_windowCycle->setView(nullptr);
    m_switcherView.reset();
    m_windowCycle.reset();
    m_inputManager.setGestureFilter(nullptr);
    m_gestureHandler.reset();
    m_deskPreferences.reset();
    m_seat.shutdown();

    // Clients go first: their surfaces own GL textures that need the context.
    if (m_syncobjGlobal)
        wl_global_destroy(m_syncobjGlobal);
    m_scanoutPolicy.reset();
    m_dmabufGlobal.reset();
    m_xwayland.reset();
    if (m_globals && m_globals->inputMethodLauncher())
        m_globals->inputMethodLauncher()->setCommand({});
    if (m_server)
        wl_display_destroy_clients(m_server->display());
    if (m_globals)
        m_globals->shutdown();
    m_outputGlobal = nullptr;
    m_scene.root()->setChangeCallback({});
    // ProtocolGlobals outlives the server here, as its objects did when they were
    // locals of main(): it is declared after m_server, so it is destroyed once
    // this body returns.
    m_server.reset();
    m_outputGlobals.clear();
    // Runtime-owned output models must outlive their Wayland resources.
    m_displayRuntime.reset();

    // Tear down the display before the renderer and the session that own it.
    if (m_backend && m_backend->primaryOutput())
        m_backend->primaryOutput()->reportStats();
    // The cursor plane belongs to the output; give it back before the backend
    // that owns it goes away.
    m_cursor.setSink(nullptr);
    m_cursorRouter.reset();
    m_scene.root()->setChangeCallback({});
    m_scene.root()->releaseRenderResources();
    // Outside the scene, so released by hand: a texture outliving the backend
    // keeps its EGL display up while the GBM device under it is destroyed.
    m_decorationTheme.clearTextures();
    m_backend.reset();
    m_session.reset();
}

void CompositorShell::onCursorChanged()
{
    if (m_globals && m_globals->imageCapture())
        m_globals->imageCapture()->cursorChanged();
}

void CompositorShell::onCursorPositionChanged()
{
    onCursorChanged();
}

void CompositorShell::onKeyboardLayoutUpdated()
{
    if (m_acceleratorController) {
        m_acceleratorController->setUsePositionalLookup(
                accelerators::usesPositionalShortcuts(*m_seat.xkbTracker()));
    }
}

} // namespace cutefish::core
