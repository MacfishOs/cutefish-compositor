// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "clipboard_test_util.h"
#include <GLES3/gl3.h>
#include <array>
#include <csignal>
#include <cutefish-window-management-v1-client-protocol.h>
#include <drm_fourcc.h>
#include <future>
#include <gbm.h>
#include <gtest/gtest.h>
#include <linux/input-event-codes.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xcb/shape.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xwayland-shell-v1-client-protocol.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>

#include "compositor/compositor.h"
#include "compositor/output.h"
#include "compositor/surface.h"
#include "decoration/decoration.h"
#include "decoration/decoration_theme.h"
#include "egl_test_util.h"
#include "input/hit_test.h"
#include "input/input_manager.h"
#include "input/pointer_constraint_delegate.h"
#include "input/pointer_constraints.h"
#include "input/seat.h"
#include "scene/scene.h"
#include "scene/surface_node.h"
#include "scene/window_animation.h"
#include "shm_test_util.h"
#include "wayland/privileged_client.h"
#include "wayland/protocols/cutefish_window_management.h"
#include "wayland/protocols/primary_selection.h"
#include "wayland/protocols/wl_compositor.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wl_seat.h"
#include "wayland/protocols/wl_shm.h"
#include "wayland/protocols/wl_subcompositor.h"
#include "wayland/protocols/xdg_decoration.h"
#include "wayland/protocols/xdg_shell.h"
#include "wayland/protocols/zwp_pointer_constraints.h"
#include "wayland/protocols/zwp_relative_pointer_manager.h"
#include "wayland/server.h"
#include "wm/activation.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_modality_controller.h"
#include "xwayland/surface_association.h"
#include "xwayland/xwayland_server.h"
#include "xwayland/xwayland_shell.h"

using namespace cutefish;

namespace {

bool until(const std::function<bool()> &predicate, int timeout = 10000)
{
    QDeadlineTimer deadline(timeout);
    while (!predicate() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

template <typename F> auto xcbReply(F operation)
{
    auto future = std::async(std::launch::async, operation);
    EXPECT_TRUE(until([&] {
        return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }));
    return future.get();
}

xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    auto *r = xcbReply([&] {
        return xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), nullptr);
    });
    const auto atom = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return atom;
}

class XwaylandTest : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char name[] = "xwayland-test";
            static char *argv[] = {name, nullptr};
            app = std::make_unique<QCoreApplication>(argc, argv);
        }
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        server = std::make_unique<wayland::Server>();
        compositor = std::make_unique<compositor::Compositor>(scene.root());
        seat = std::make_unique<input::Seat>(compositor.get());
        manager = std::make_unique<wm::WindowManager>(compositor.get(), seat.get());
        manager->setOutputBounds(QRect(0, 0, 800, 600));
        manager->setFrameProvider(&frames);
        privileged = std::make_unique<wayland::PrivilegedClients>(server->display());
        waylandSeat = std::make_unique<wayland::WaylandSeat>(seat.get(), server->serialTracker());
        output.pixelSize = QSize(800, 600);
        ASSERT_TRUE(wayland::createCompositorGlobal(server->display(), compositor.get()));
        ASSERT_TRUE(wayland::createShmGlobal(server->display(), compositor.get()));
        ASSERT_TRUE(wayland::createSubcompositorGlobal(server->display()));
        outputGlobal = wayland::createOutputGlobal(server->display(), &output);
        ASSERT_TRUE(outputGlobal);
        ASSERT_TRUE(wayland::createXdgOutputManagerGlobal(server->display()));
        ASSERT_TRUE(wayland::createSeatGlobal(server->display(), waylandSeat.get()));
        ASSERT_TRUE(wayland::createPointerConstraintsGlobal(server->display(), waylandSeat.get()));
        ASSERT_TRUE(wayland::createRelativePointerManagerGlobal(server->display()));
        ASSERT_TRUE(server->start());
        xwayland = std::make_unique<xwayland::XwaylandServer>(server->display(), privileged.get(),
                                                              manager.get());
    }

    void TearDown() override
    {
        xwayland.reset();
        wl_display_destroy_clients(server->display());
        waylandSeat.reset();
        privileged.reset();
        server.reset();
        manager.reset();
        seat.reset();
        compositor.reset();
    }

    bool start(int restartLimit = 3)
    {
        xwayland::XwaylandServer::Options options;
        options.software = true;
        options.readinessFile = runtime.path() + "/ready.json";
        options.restartLimit = restartLimit;
        options.pingTimeoutMs = pingTimeoutMs;
        options.windowLimit = windowLimit;
        if (!xwayland->start(options))
            return false;
        return until([&] {
                   return xwayland->isReady() || !xwayland->error().isEmpty();
               })
                && xwayland->isReady();
    }

    std::unique_ptr<QCoreApplication> app;
    int pingTimeoutMs = 5000;
    size_t windowLimit = xwayland::SurfaceAssociation::limit;
    QTemporaryDir runtime;
    scene::Scene scene;
    decoration::Theme theme;
    decoration::DecorationProvider frames {&theme};
    compositor::Output output;
    wl_global *outputGlobal = nullptr;
    std::unique_ptr<wayland::Server> server;
    std::unique_ptr<compositor::Compositor> compositor;
    std::unique_ptr<input::Seat> seat;
    std::unique_ptr<wm::WindowManager> manager;
    std::unique_ptr<wayland::PrivilegedClients> privileged;
    std::unique_ptr<wayland::WaylandSeat> waylandSeat;
    std::unique_ptr<xwayland::XwaylandServer> xwayland;
};

TEST(SurfaceAssociation, WordsOrdersAndRetirement)
{
    compositor::Compositor compositor(nullptr);
    auto first = compositor.createSurface();
    auto second = compositor.createSurface();
    xwayland::SurfaceAssociation association;
    uint32_t matched = 0;
    association.associated = [&](uint32_t id, auto *surface) {
        matched = id;
        EXPECT_TRUE(surface == first.get() || surface == second.get());
    };
    const auto serial = association.serial(0xfedcba98, 0x12345678);
    EXPECT_EQ(serial, 0x12345678fedcba98ULL);
    EXPECT_TRUE(association.addWindow(serial, 42));
    EXPECT_EQ(matched, 0u);
    EXPECT_TRUE(association.claimSurfaceSerial(serial));
    EXPECT_TRUE(association.addSurface(serial, first.get()));
    EXPECT_EQ(matched, 42u);
    EXPECT_FALSE(association.addWindow(serial, 99));
    EXPECT_FALSE(association.claimSurfaceSerial(serial));
    // Serials only increase on each side.
    EXPECT_FALSE(association.addWindow(2, 43));
    EXPECT_FALSE(association.claimSurfaceSerial(2));
    EXPECT_TRUE(association.claimSurfaceSerial(serial + 1));
    EXPECT_TRUE(association.addSurface(serial + 1, second.get()));
    EXPECT_TRUE(association.addWindow(serial + 1, 43));
    EXPECT_EQ(matched, 43u);
    EXPECT_FALSE(association.addWindow(0, 44));
    EXPECT_FALSE(association.claimSurfaceSerial(0));
    EXPECT_EQ(association.pendingCount(), 0u);
}

TEST(SurfaceAssociation, DestructionRemapAndNewGeneration)
{
    compositor::Compositor compositor(nullptr);
    auto surface = compositor.createSurface();
    xwayland::SurfaceAssociation association;
    int matches = 0;
    association.associated = [&](auto, auto *) {
        ++matches;
    };
    ASSERT_TRUE(association.addWindow(10, 42));
    association.removeWindow(42);
    EXPECT_FALSE(association.addWindow(10, 42));
    ASSERT_TRUE(association.claimSurfaceSerial(10));
    ASSERT_TRUE(association.addSurface(10, surface.get()));
    EXPECT_EQ(matches, 0);
    association.removeSurface(surface.get());
    EXPECT_FALSE(association.claimSurfaceSerial(10));
    ASSERT_TRUE(association.addWindow(11, 42));
    ASSERT_TRUE(association.claimSurfaceSerial(11));
    ASSERT_TRUE(association.addSurface(11, surface.get()));
    EXPECT_EQ(matches, 1);
    EXPECT_EQ(association.pendingCount(), 0u);
    xwayland::SurfaceAssociation next;
    EXPECT_TRUE(next.claimSurfaceSerial(10));
    EXPECT_TRUE(next.addWindow(10, 42));
}

// Xwayland takes a new serial for every map, so a long session keeps going
// past any count of them.
TEST(SurfaceAssociation, SerialsDoNotRunOutInALongSession)
{
    compositor::Compositor compositor(nullptr);
    auto surface = compositor.createSurface();
    xwayland::SurfaceAssociation association;
    uint64_t matches = 0;
    association.associated = [&](auto, auto *) {
        ++matches;
    };
    const uint64_t maps = xwayland::SurfaceAssociation::limit + 16;
    for (uint64_t serial = 1; serial <= maps; ++serial) {
        ASSERT_TRUE(association.addWindow(serial, 42)) << serial;
        ASSERT_TRUE(association.claimSurfaceSerial(serial)) << serial;
        ASSERT_TRUE(association.addSurface(serial, surface.get())) << serial;
    }
    EXPECT_EQ(matches, maps);
    EXPECT_EQ(association.pendingCount(), 0u);
}

TEST_F(XwaylandTest, MissingExecutable)
{
    xwayland::XwaylandServer::Options options;
    options.executable = "/nonexistent/cutefish-Xwayland";
    ASSERT_TRUE(xwayland->start(options));
    ASSERT_TRUE(until([&] {
        return !xwayland->error().isEmpty();
    }));
    EXPECT_FALSE(xwayland->isReady());
    EXPECT_EQ(server->trackedClientCount(), 0);
}

TEST_F(XwaylandTest, EarlyExit)
{
    xwayland::XwaylandServer::Options options;
    options.executable = "/bin/false";
    ASSERT_TRUE(xwayland->start(options));
    ASSERT_TRUE(until([&] {
        return !xwayland->error().isEmpty();
    }));
    EXPECT_FALSE(xwayland->isReady());
}

TEST_F(XwaylandTest, MalformedPartialAndTimeout)
{
    for (const auto *mode : {"partial", "malformed", "timeout"}) {
        qputenv("CUTEFISH_XWAYLAND_FAULT", mode);
        xwayland::XwaylandServer::Options options;
        options.executable = XWAYLAND_FAULT_PATH;
        options.timeoutMs = 100;
        ASSERT_TRUE(xwayland->start(options));
        ASSERT_TRUE(until([&] {
            return !xwayland->error().isEmpty();
        }));
        EXPECT_FALSE(xwayland->isReady());
        EXPECT_EQ(server->trackedClientCount(), 0);
    }
    qunsetenv("CUTEFISH_XWAYLAND_FAULT");
}

TEST_F(XwaylandTest, RealStartupRepeatedAndCrash)
{
    if (!QFile::exists("/usr/bin/Xwayland"))
        GTEST_SKIP() << "system Xwayland unavailable";
    int nativeSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, nativeSockets), 0);
    auto *nativeServer = wl_client_create(server->display(), nativeSockets[0]);
    auto *nativeClient = wl_display_connect_to_fd(nativeSockets[1]);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(start()) << xwayland->error().toStdString();
        EXPECT_TRUE(QFile::exists(runtime.path() + "/ready.json"));
        EXPECT_FALSE(privileged->isTrusted(privileged->shellClient()));
        if (i == 1) {
            // A crash restarts on the same display with the same Xauthority, so
            // the environment the session exported stays valid.
            const auto display = xwayland->display();
            const auto authority = xwayland->authority();
            const auto pid = xwayland->processId();
            kill(pid, SIGKILL);
            ASSERT_TRUE(until([&] {
                return xwayland->isReady() && xwayland->processId() != pid;
            })) << xwayland->error().toStdString();
            EXPECT_TRUE(xwayland->error().isEmpty());
            EXPECT_EQ(xwayland->display(), display);
            EXPECT_EQ(xwayland->authority(), authority);
            EXPECT_TRUE(QFile::exists(runtime.path() + "/ready.json"));
            qputenv("XAUTHORITY", authority.toUtf8());
            auto *c = xcbReply([&] {
                return xcb_connect(display.toUtf8().constData(), nullptr);
            });
            EXPECT_EQ(xcb_connection_has_error(c), 0);
            xcb_disconnect(c);
        }
        xwayland->stop();
        EXPECT_FALSE(xwayland->isReady());
        EXPECT_FALSE(QFile::exists(runtime.path() + "/ready.json"));
        EXPECT_EQ(server->trackedClientCount(), 1);
        auto sync = std::async(std::launch::async, [&] {
            return wl_display_roundtrip(nativeClient);
        });
        ASSERT_TRUE(until([&] {
            return sync.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }));
        EXPECT_GE(sync.get(), 0);
        EXPECT_TRUE(manager->windows().empty());
    }
    wl_display_disconnect(nativeClient);
    wl_client_destroy(nativeServer);
}

TEST_F(XwaylandTest, RepeatedCrashesGiveUpAndWithdrawTheDisplay)
{
    if (!QFile::exists("/usr/bin/Xwayland"))
        GTEST_SKIP() << "system Xwayland unavailable";
    ASSERT_TRUE(start(1)) << xwayland->error().toStdString();
    QString failure;
    xwayland->failure = [&](const QString &error) {
        failure = error;
    };
    auto pid = xwayland->processId();
    kill(pid, SIGKILL);
    ASSERT_TRUE(until([&] {
        return xwayland->isReady() && xwayland->processId() != pid;
    }));
    EXPECT_TRUE(failure.isEmpty());
    // The one restart a minute is used up, so the next crash is final.
    kill(xwayland->processId(), SIGKILL);
    ASSERT_TRUE(until([&] {
        return !xwayland->error().isEmpty();
    }));
    EXPECT_FALSE(failure.isEmpty());
    EXPECT_FALSE(xwayland->isReady());
    EXPECT_TRUE(xwayland->display().isEmpty());
    EXPECT_FALSE(QFile::exists(runtime.path() + "/ready.json"));
}

// A client holding the most windows is disconnected at the limit; the other
// clients, and Xwayland itself, carry on.
TEST_F(XwaylandTest, WindowLimitDisconnectsOnlyTheGreediestClient)
{
    windowLimit = 16;
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    const auto pid = xwayland->processId();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    const auto connect = [&] {
        return xcbReply([&] {
            return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
        });
    };
    auto *greedy = connect();
    auto *modest = connect();
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(greedy);
        xcb_disconnect(modest);
    });
    ASSERT_EQ(xcb_connection_has_error(greedy), 0);
    ASSERT_EQ(xcb_connection_has_error(modest), 0);
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(modest)).data;
    const auto create = [&](xcb_connection_t *c) {
        xcb_create_window(c, XCB_COPY_FROM_PARENT, xcb_generate_id(c), screen->root, 0, 0, 10,
                          10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        xcb_flush(c);
    };
    const auto answers = [&](xcb_connection_t *c) {
        auto *reply = xcbReply([&] {
            return xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
        });
        free(reply);
        return reply != nullptr;
    };
    create(modest);
    ASSERT_TRUE(answers(modest));
    for (int i = 0; i < 40; ++i)
        create(greedy);
    ASSERT_TRUE(until([&] {
        return !answers(greedy);
    }));
    create(modest);
    until([] {
        return false;
    }, 200);
    EXPECT_TRUE(answers(modest));
    EXPECT_TRUE(xwayland->isReady());
    EXPECT_EQ(pid, xwayland->processId());
}

// A window moved into another is forgotten, since its DestroyNotify goes to the
// new parent, and never counts against the limit; moved back, it is followed again.
TEST_F(XwaylandTest, AWindowMovedOffTheRootIsForgottenAndAdoptedWhenItReturns)
{
    windowLimit = 8;
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    const auto pid = xwayland->processId();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto create = [&] {
        const auto id = xcb_generate_id(c);
        const uint32_t values[] = {screen->white_pixel};
        xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL,
                          values);
        return id;
    };
    const auto embedder = create();
    for (int i = 0; i < 16; ++i) {
        const auto plug = create();
        xcb_reparent_window(c, plug, embedder, 0, 0);
        xcb_destroy_window(c, plug);
    }
    const auto returning = create();
    xcb_reparent_window(c, returning, embedder, 0, 0);
    xcb_reparent_window(c, returning, screen->root, 30, 40);
    // A restack and a move before its size is read keep the window's own size.
    const uint32_t above[] = {XCB_STACK_MODE_ABOVE};
    xcb_configure_window(c, returning, XCB_CONFIG_WINDOW_STACK_MODE, above);
    const uint32_t position[] = {50, 60};
    xcb_configure_window(c, returning, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, position);
    xcb_map_window(c, returning);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1
                && manager->windows()[0]->rootSurface()->contentSize() == QSize(200, 100);
    }));
    auto *geometry = xcbReply([&] {
        return xcb_get_geometry_reply(c, xcb_get_geometry(c, returning), nullptr);
    });
    const QSize size = geometry ? QSize(geometry->width, geometry->height) : QSize();
    free(geometry);
    EXPECT_EQ(size, QSize(200, 100));
    auto *reply = xcbReply([&] {
        return xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
    });
    EXPECT_TRUE(reply);
    free(reply);
    EXPECT_TRUE(xwayland->isReady());
    EXPECT_EQ(pid, xwayland->processId());
}

// Serials only increase, so a client sending a WL_SURFACE_SERIAL far ahead of
// Xwayland's would leave every later window unmatched but for the SendEvent check.
TEST_F(XwaylandTest, ASurfaceSerialSentByAClientIsIgnored)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL,
                      values);
    xcb_client_message_event_t forged {};
    forged.response_type = XCB_CLIENT_MESSAGE;
    forged.format = 32;
    forged.window = id;
    forged.type = intern(c, "WL_SURFACE_SERIAL");
    forged.data.data32[0] = 0xffffffff;
    forged.data.data32[1] = 0xffffffff;
    xcb_send_event(c, 0, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   reinterpret_cast<const char *>(&forged));
    xcb_map_window(c, id);
    xcb_flush(c);
    EXPECT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
}

// Xwayland is fully dead before the event loop runs again, so its exit and both
// lost connections are all reported in one iteration: one crash, one restart.
TEST_F(XwaylandTest, ACrashReportedOnEveryChannelAtOnceCountsOnce)
{
    ASSERT_TRUE(start(1)) << xwayland->error().toStdString();
    const auto pid = xwayland->processId();
    ASSERT_EQ(kill(pid, SIGKILL), 0);
    const QString stat = QStringLiteral("/proc/%1/stat").arg(pid);
    QDeadlineTimer dead(5000);
    for (;;) {
        QFile file(stat);
        if (!file.open(QIODevice::ReadOnly))
            break;
        const QByteArray line = file.readAll();
        const int state = line.lastIndexOf(')') + 2;
        if (state > 1 && state < line.size() && line[state] == 'Z')
            break;
        ASSERT_FALSE(dead.hasExpired());
        QThread::msleep(5);
    }
    ASSERT_TRUE(until([&] {
        return xwayland->isReady() && xwayland->processId() != pid;
    })) << xwayland->error().toStdString();
    EXPECT_TRUE(xwayland->error().isEmpty());
}

TEST_F(XwaylandTest, ScaledX11ClientsDrawAtDeviceResolution)
{
    // Scale 2: the X11 screen, windows and pointer are in device pixels, and
    // the logical window is half as large.
    xwayland->setScale(2);
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    auto rootSize = [&] {
        auto *r = xcbReply([&] {
            return xcb_get_geometry_reply(c, xcb_get_geometry(c, screen->root), nullptr);
        });
        const QSize size = r ? QSize(r->width, r->height) : QSize();
        free(r);
        return size;
    };
    ASSERT_TRUE(until([&] {
        return rootSize() == QSize(1600, 1200);
    }));
    auto cardinals = [&](const char *name) {
        std::vector<uint32_t> values;
        auto *r = xcbReply([&] {
            return xcb_get_property_reply(
                    c, xcb_get_property(c, 0, screen->root, intern(c, name), XCB_ATOM_CARDINAL, 0, 8),
                    nullptr);
        });
        if (r) {
            auto *words = static_cast<uint32_t *>(xcb_get_property_value(r));
            values.assign(words, words + xcb_get_property_value_length(r) / 4);
        }
        free(r);
        return values;
    };
    EXPECT_EQ(cardinals("_NET_DESKTOP_GEOMETRY"), (std::vector<uint32_t> {1600, 1200}));
    EXPECT_EQ(cardinals("_NET_WORKAREA"), (std::vector<uint32_t> {0, 0, 1600, 1200}));
    auto resources = [&] {
        auto *r = xcbReply([&] {
            return xcb_get_property_reply(c,
                                          xcb_get_property(c, 0, screen->root,
                                                           XCB_ATOM_RESOURCE_MANAGER,
                                                           XCB_ATOM_STRING, 0, 4096),
                                          nullptr);
        });
        QByteArray value;
        if (r)
            value = QByteArray(static_cast<const char *>(xcb_get_property_value(r)),
                               xcb_get_property_value_length(r));
        free(r);
        return value;
    };
    ASSERT_TRUE(until([&] {
        return resources().contains("Xft.dpi:\t192");
    }));
    auto *owner = xcbReply([&] {
        return xcb_get_selection_owner_reply(
                c, xcb_get_selection_owner(c, intern(c, "_XSETTINGS_S0")), nullptr);
    });
    ASSERT_TRUE(owner && owner->owner);
    const xcb_window_t settingsWindow = owner->owner;
    free(owner);
    auto xsettings = [&] {
        auto *r = xcbReply([&] {
            const auto atom = intern(c, "_XSETTINGS_SETTINGS");
            return xcb_get_property_reply(
                    c, xcb_get_property(c, 0, settingsWindow, atom, atom, 0, 1024), nullptr);
        });
        QByteArray value;
        if (r)
            value = QByteArray(static_cast<const char *>(xcb_get_property_value(r)),
                               xcb_get_property_value_length(r));
        free(r);
        return value;
    };
    // The window scaling factor is the INT32 after its name and serial.
    auto setting = [&](const QByteArray &data, const QByteArray &name) {
        const auto at = data.indexOf(name);
        if (at < 0)
            return -1;
        const int value = at + ((name.size() + 3) & ~3) + 4;
        return *reinterpret_cast<const int32_t *>(data.constData() + value);
    };
    EXPECT_EQ(setting(xsettings(), "Gdk/WindowScalingFactor"), 2);
    EXPECT_EQ(setting(xsettings(), "Xft/DPI"), 192 * 1024);

    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_BUTTON_PRESS};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 600, 400, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1
                && manager->windows()[0]->rootSurface()->contentSize() == QSize(300, 200);
    }));
    auto *window = manager->windows()[0];
    EXPECT_EQ(window->contentBounds().size(), QSize(300, 200));
    EXPECT_EQ(window->rootSurface()->bufferSize(), QSize(600, 400));

    // A logical point lands on the X11 pixel under it.
    const QPoint inside = window->contentBounds().topLeft() + QPoint(100, 50);
    seat->onPointerMotion(std::chrono::steady_clock::now(), inside);
    seat->onPointerButton(std::chrono::steady_clock::now(), inside, BTN_LEFT, true);
    seat->onPointerButton(std::chrono::steady_clock::now(), inside, BTN_LEFT, false);
    QPoint pressed(-1, -1);
    ASSERT_TRUE(until([&] {
        while (auto *e = xcb_poll_for_event(c)) {
            if ((e->response_type & 0x7f) == XCB_BUTTON_PRESS) {
                auto *press = reinterpret_cast<xcb_button_press_event_t *>(e);
                pressed = QPoint(press->event_x, press->event_y);
            }
            free(e);
        }
        return pressed != QPoint(-1, -1);
    }));
    EXPECT_EQ(pressed, QPoint(200, 100));

    // Back to scale 1 at run time: the screen, the window and Xft.dpi follow.
    xwayland->setScale(1);
    wayland::updateOutputGlobal(outputGlobal, &output);
    ASSERT_TRUE(until([&] {
        return rootSize() == QSize(800, 600);
    }));
    auto windowSize = [&] {
        auto *r = xcbReply([&] {
            return xcb_get_geometry_reply(c, xcb_get_geometry(c, id), nullptr);
        });
        const QSize size = r ? QSize(r->width, r->height) : QSize();
        free(r);
        return size;
    };
    ASSERT_TRUE(until([&] {
        return windowSize() == QSize(300, 200)
                && window->rootSurface()->contentSize() == QSize(300, 200);
    }));
    EXPECT_TRUE(until([&] {
        return resources().contains("Xft.dpi:\t96");
    }));
    EXPECT_EQ(setting(xsettings(), "Gdk/WindowScalingFactor"), 1);
}

TEST_F(XwaylandTest, WindowProcessIsTheLocalClientBehindTheConnection)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    // A forwarded client, as with ssh -X, names the machine it runs on and
    // reaches the server through a local proxy, whose pid is not the client's.
    for (const bool forwarded : {false, true}) {
        QProcess client;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("DISPLAY", xwayland->display());
        env.insert("XAUTHORITY", xwayland->authority());
        client.setProcessEnvironment(env);
        client.start(X11_TEST_CLIENT,
                     {forwarded ? QStringLiteral("elsewhere.invalid") : QSysInfo::machineHostName()});
        ASSERT_TRUE(until([&] {
            return manager->windows().size() == 1 || client.state() == QProcess::NotRunning;
        }));
        ASSERT_EQ(manager->windows().size(), 1u) << client.readAllStandardError().toStdString();
        EXPECT_EQ(manager->windows()[0]->processId(), forwarded ? 0 : client.processId());

        client.kill();
        ASSERT_TRUE(until([&] {
            return client.state() == QProcess::NotRunning && manager->windows().empty();
        }));
    }
}

TEST_F(XwaylandTest, UnansweredNetWmPingMarksTheWindowAndCloseKillsTheClient)
{
    pingTimeoutMs = 100;
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL,
                      values);
    const xcb_atom_t protocols[] = {intern(c, "WM_DELETE_WINDOW"), intern(c, "_NET_WM_PING")};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, intern(c, "WM_PROTOCOLS"), XCB_ATOM_ATOM, 32,
                        std::size(protocols), protocols);
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    auto *window = manager->windows()[0];
    const xcb_atom_t wmProtocols = intern(c, "WM_PROTOCOLS");
    const xcb_atom_t netWmPing = intern(c, "_NET_WM_PING");
    std::vector<xcb_client_message_event_t> pings;
    int deletes = 0;
    auto poll = [&] {
        while (auto *e = xcb_poll_for_event(c)) {
            if ((e->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                auto *m = reinterpret_cast<xcb_client_message_event_t *>(e);
                if (m->type == wmProtocols && m->data.data32[0] == netWmPing)
                    pings.push_back(*m);
                else if (m->type == wmProtocols)
                    ++deletes;
            }
            free(e);
        }
    };
    auto pong = [&](xcb_client_message_event_t message) {
        message.window = screen->root;
        xcb_send_event(c, 0, screen->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                       reinterpret_cast<const char *>(&message));
        xcb_flush(c);
    };

    // An answered ping keeps the window responsive past the timeout.
    window->ping();
    ASSERT_TRUE(until([&] {
        poll();
        return pings.size() == 1;
    }));
    EXPECT_EQ(pings[0].window, id);
    EXPECT_EQ(pings[0].data.data32[2], id);
    pong(pings[0]);
    QDeadlineTimer settle(300);
    while (!settle.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_TRUE(window->isResponsive());

    // An unanswered one turns it unresponsive; a late answer still counts.
    window->ping();
    ASSERT_TRUE(until([&] {
        poll();
        return pings.size() == 2 && !window->isResponsive();
    }));
    pong(pings[1]);
    ASSERT_TRUE(until([&] {
        return window->isResponsive();
    }));

    // A close then kills the client instead of asking it.
    window->ping();
    ASSERT_TRUE(until([&] {
        poll();
        return pings.size() == 3 && !window->isResponsive();
    }));
    window->requestClose();
    ASSERT_TRUE(until([&] {
        poll();
        return xcb_connection_has_error(c) != 0;
    }));
    EXPECT_EQ(deletes, 0);
    EXPECT_TRUE(until([&] {
        return manager->windows().empty();
    }));
}

TEST_F(XwaylandTest, NetWmStateAboveAndSkipTaskbarReachTheWindow)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL,
                      values);
    const xcb_atom_t netWmState = intern(c, "_NET_WM_STATE");
    const xcb_atom_t above = intern(c, "_NET_WM_STATE_ABOVE");
    const xcb_atom_t skipTaskbar = intern(c, "_NET_WM_STATE_SKIP_TASKBAR");
    const xcb_atom_t initial[] = {above, skipTaskbar};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, netWmState, XCB_ATOM_ATOM, 32,
                        std::size(initial), initial);
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    auto *window = manager->windows()[0];
    EXPECT_TRUE(window->isAlwaysOnTop());
    EXPECT_TRUE(window->skipTaskbar());

    auto states = [&] {
        std::vector<xcb_atom_t> result;
        auto *r = xcbReply([&] {
            return xcb_get_property_reply(
                    c, xcb_get_property(c, 0, id, netWmState, XCB_ATOM_ATOM, 0, 32), nullptr);
        });
        if (r) {
            auto *atoms = static_cast<xcb_atom_t *>(xcb_get_property_value(r));
            result.assign(atoms, atoms + xcb_get_property_value_length(r) / 4);
        }
        free(r);
        return result;
    };
    auto has = [](const std::vector<xcb_atom_t> &list, xcb_atom_t atom) {
        return std::find(list.begin(), list.end(), atom) != list.end();
    };
    auto request = [&](uint32_t action, xcb_atom_t state) {
        xcb_client_message_event_t e {};
        e.response_type = XCB_CLIENT_MESSAGE;
        e.format = 32;
        e.window = id;
        e.type = netWmState;
        e.data.data32[0] = action;
        e.data.data32[1] = state;
        e.data.data32[3] = 1;
        xcb_send_event(c, 0, screen->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                       reinterpret_cast<const char *>(&e));
        xcb_flush(c);
    };
    ASSERT_TRUE(until([&] {
        const auto list = states();
        return has(list, above) && has(list, skipTaskbar);
    }));

    request(0, above);
    ASSERT_TRUE(until([&] {
        return !window->isAlwaysOnTop();
    }));
    request(2, skipTaskbar);
    ASSERT_TRUE(until([&] {
        return !window->skipTaskbar();
    }));
    ASSERT_TRUE(until([&] {
        const auto list = states();
        return !has(list, above) && !has(list, skipTaskbar);
    }));
    request(1, above);
    EXPECT_TRUE(until([&] {
        return window->isAlwaysOnTop() && has(states(), above);
    }));
}

TEST_F(XwaylandTest, NetWmWindowOpacityFadesTheWholeWindow)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL,
                      values);
    // Xwayland Video Bridge maps a window this way to stay unseen.
    const xcb_atom_t opacity = intern(c, "_NET_WM_WINDOW_OPACITY");
    const uint32_t transparent = 0;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, opacity, XCB_ATOM_CARDINAL, 32, 1,
                        &transparent);
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    auto *node = dynamic_cast<scene::WindowAnimation *>(manager->windows()[0]->node());
    ASSERT_TRUE(node);
    EXPECT_FLOAT_EQ(node->opacity(), 0);

    const uint32_t half = 0x80000000u;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, opacity, XCB_ATOM_CARDINAL, 32, 1, &half);
    xcb_flush(c);
    EXPECT_TRUE(until([&] {
        return std::abs(node->opacity() - .5f) < 1e-3f;
    }));
    xcb_delete_property(c, id, opacity);
    xcb_flush(c);
    EXPECT_TRUE(until([&] {
        return node->opacity() == 1;
    }));
}

TEST_F(XwaylandTest, OnlyAChangedFollowedPropertyIsReadAndNothingIsWrittenTwice)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel,
                               XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const xcb_atom_t name = intern(c, "_NET_WM_NAME");
    const xcb_atom_t utf8 = intern(c, "UTF8_STRING");
    const xcb_atom_t userTime = intern(c, "_NET_WM_USER_TIME");
    const std::vector<xcb_atom_t> written = {intern(c, "_NET_WM_STATE"), intern(c, "WM_STATE"),
                                             intern(c, "_NET_FRAME_EXTENTS")};
    const auto setName = [&](const QByteArray &title) {
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, name, utf8, 8, title.size(),
                            title.constData());
        xcb_flush(c);
    };
    setName("first");
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1 && manager->windows()[0]->title() == "first";
    }));
    auto *window = manager->windows()[0];
    int configures = 0, writes = 0;
    const auto drain = [&] {
        while (auto *event = xcb_poll_for_event(c)) {
            const auto type = event->response_type & ~0x80;
            if (type == XCB_CONFIGURE_NOTIFY)
                ++configures;
            if (type == XCB_PROPERTY_NOTIFY) {
                const auto atom = reinterpret_cast<xcb_property_notify_event_t *>(event)->atom;
                writes += std::find(written.begin(), written.end(), atom) != written.end();
            }
            free(event);
        }
    };
    until([&] {
        drain();
        return false;
    }, 300);
    configures = writes = 0;

    // Key presses in GTK set the user time; the title is what a taskbar shows.
    for (uint32_t time = 1; time <= 5; ++time) {
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, userTime, XCB_ATOM_CARDINAL, 32, 1,
                            &time);
    }
    setName("second");
    ASSERT_TRUE(until([&] {
        drain();
        return window->title() == "second";
    }));
    until([&] {
        drain();
        return false;
    }, 300);
    EXPECT_EQ(configures, 0);
    EXPECT_EQ(writes, 0);

    // A move tells the client where it is, without rewriting its state.
    const QRect bounds = window->bounds();
    window->setBounds(bounds.translated(10, 0));
    ASSERT_TRUE(until([&] {
        drain();
        return configures > 0;
    }));
    until([&] {
        drain();
        return false;
    }, 300);
    EXPECT_EQ(writes, 0);
}

TEST_F(XwaylandTest, OnDemandServerStartsForTheFirstClient)
{
    if (!QFile::exists("/usr/bin/Xwayland"))
        GTEST_SKIP() << "system Xwayland unavailable";
    xwayland::XwaylandServer::Options options;
    options.software = true;
    options.lazy = true;
    options.readinessFile = runtime.path() + "/ready.json";
    int started = 0;
    xwayland->started = [&] {
        ++started;
    };
    ASSERT_TRUE(xwayland->start(options)) << xwayland->error().toStdString();
    // The display is the session's at once, with nothing running behind it.
    const QString display = xwayland->display();
    ASSERT_FALSE(display.isEmpty());
    EXPECT_TRUE(QFile::exists(runtime.path() + "/ready.json"));
    const QString lock = "/tmp/.X" + display.mid(1) + "-lock";
    const QString socket = "/tmp/.X11-unix/X" + display.mid(1);
    EXPECT_TRUE(QFile::exists(lock));
    EXPECT_TRUE(QFile::exists(socket));
    QDeadlineTimer idle(300);
    while (!idle.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_FALSE(xwayland->isReady());
    EXPECT_EQ(xwayland->processId(), 0);
    EXPECT_EQ(started, 0);

    // The first client's connection is the one Xwayland accepts.
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    for (int round = 0; round < 2; ++round) {
        SCOPED_TRACE(round);
        auto *c = xcbReply([&] {
            return xcb_connect(display.toUtf8().constData(), nullptr);
        });
        ASSERT_EQ(xcb_connection_has_error(c), 0);
        EXPECT_TRUE(xwayland->isReady());
        EXPECT_EQ(started, round + 1);
        auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
        const auto id = xcb_generate_id(c);
        const uint32_t values[] = {screen->white_pixel};
        xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL,
                          values);
        xcb_map_window(c, id);
        xcb_flush(c);
        EXPECT_TRUE(until([&] {
            return manager->windows().size() == 1;
        }));
        xcb_disconnect(c);
        EXPECT_TRUE(until([&] {
            return manager->windows().empty();
        }));
        if (round == 0) {
            // A crash goes back to waiting for a client on the same display.
            kill(xwayland->processId(), SIGKILL);
            ASSERT_TRUE(until([&] {
                return !xwayland->isReady() && xwayland->processId() == 0;
            }));
            EXPECT_TRUE(xwayland->error().isEmpty());
            EXPECT_EQ(xwayland->display(), display);
            EXPECT_TRUE(QFile::exists(runtime.path() + "/ready.json"));
        }
    }
    xwayland->stop();
    EXPECT_FALSE(QFile::exists(lock));
    EXPECT_FALSE(QFile::exists(socket));
    EXPECT_FALSE(QFile::exists(runtime.path() + "/ready.json"));
}

TEST_F(XwaylandTest, NetWmSyncRequestHoldsResizesForTheClientCounter)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    free(xcbReply([&] {
        return xcb_sync_initialize_reply(c, xcb_sync_initialize(c, 3, 1), nullptr);
    }));
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const xcb_sync_counter_t counter = xcb_generate_id(c);
    xcb_sync_create_counter(c, counter, {0, 5});
    const xcb_atom_t syncRequest = intern(c, "_NET_WM_SYNC_REQUEST");
    const xcb_atom_t protocols[] = {syncRequest};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, intern(c, "WM_PROTOCOLS"), XCB_ATOM_ATOM, 32,
                        1, protocols);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, intern(c, "_NET_WM_SYNC_REQUEST_COUNTER"),
                        XCB_ATOM_CARDINAL, 32, 1, &counter);
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    auto *window = manager->windows()[0];
    // The window manager starts the count from zero.
    ASSERT_TRUE(until([&] {
        auto *r = xcbReply([&] {
            return xcb_sync_query_counter_reply(c, xcb_sync_query_counter(c, counter), nullptr);
        });
        const bool zero = r && r->counter_value.hi == 0 && r->counter_value.lo == 0;
        free(r);
        return zero;
    }));

    std::vector<uint32_t> requests;
    std::vector<QSize> configures;
    const xcb_atom_t wmProtocols = intern(c, "WM_PROTOCOLS");
    auto poll = [&] {
        while (auto *e = xcb_poll_for_event(c)) {
            const auto type = e->response_type & 0x7f;
            if (type == XCB_CLIENT_MESSAGE) {
                auto *m = reinterpret_cast<xcb_client_message_event_t *>(e);
                if (m->type == wmProtocols && m->data.data32[0] == syncRequest) {
                    EXPECT_TRUE(configures.size() == requests.size());
                    requests.push_back(m->data.data32[2]);
                }
            } else if (type == XCB_CONFIGURE_NOTIFY) {
                auto *n = reinterpret_cast<xcb_configure_notify_event_t *>(e);
                if (n->window == id
                    && (configures.empty() || configures.back() != QSize(n->width, n->height)))
                    configures.emplace_back(n->width, n->height);
            }
            free(e);
        }
    };
    auto resize = [&](const QSize &size) {
        const auto insets = window->frameInsets();
        window->setBounds(QRect(window->bounds().topLeft(), insets.outset(size)));
    };
    auto settle = [&](int ms) {
        QDeadlineTimer deadline(ms);
        while (!deadline.hasExpired()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            poll();
        }
    };
    settle(100);
    configures.clear();

    // A request precedes the new size.
    resize(QSize(240, 120));
    ASSERT_TRUE(until([&] {
        poll();
        return !configures.empty() && configures.back() == QSize(240, 120);
    }));
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], 1u);

    // Until the counter reaches it, further sizes wait, and only the last goes.
    resize(QSize(260, 130));
    resize(QSize(280, 140));
    settle(200);
    EXPECT_EQ(configures.back(), QSize(240, 120));
    EXPECT_EQ(requests.size(), 1u);
    xcb_sync_set_counter(c, counter, {0, requests[0]});
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        poll();
        return configures.back() == QSize(280, 140);
    }));
    EXPECT_EQ(requests.size(), 2u);

    // A client that never answers holds a resize back for a second at most.
    resize(QSize(300, 150));
    settle(300);
    EXPECT_EQ(configures.back(), QSize(280, 140));
    ASSERT_TRUE(until([&] {
        poll();
        return configures.back() == QSize(300, 150);
    }));
}

// Xwayland confines the pointer for a grab with confine_to, emulates
// XWarpPointer with a lock and relative motion, and turns a keyboard grab into
// a shortcuts inhibitor. At scale 2, X11 units are half the logical ones.
TEST_F(XwaylandTest, X11PointerGrabsWarpsAndKeyboardGrabsUseTheWaylandProtocols)
{
    xwayland->setScale(2);
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    // Without a visible cursor Xwayland locks rather than confines: that is how
    // it serves hidden-cursor panning.
    const auto font = xcb_generate_id(c);
    xcb_open_font(c, font, 6, "cursor");
    const auto cursor = xcb_generate_id(c);
    xcb_create_glyph_cursor(c, cursor, font, font, 68, 69, 0, 0, 0, 0xffff, 0xffff, 0xffff);
    xcb_close_font(c, font);
    const auto id = xcb_generate_id(c);
    const uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_POINTER_MOTION, cursor};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 600, 400, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_CURSOR, values);
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1 && manager->windows()[0]->rootSurface()->isMapped();
    }));
    auto *window = manager->windows()[0];
    window->activate();
    input::InputManager input(seat.get());
    input.setBounds(QSize(800, 600));
    auto pointerInWindow = [&] {
        auto *r = xcbReply([&] {
            return xcb_query_pointer_reply(c, xcb_query_pointer(c, id), nullptr);
        });
        const QPoint position = r ? QPoint(r->win_x, r->win_y) : QPoint(-1, -1);
        free(r);
        return position;
    };
    // The window manager places the window after it maps; entering before X11
    // has the final position would report a stale one.
    ASSERT_TRUE(until([&] {
        auto *r = xcbReply([&] {
            return xcb_translate_coordinates_reply(
                    c, xcb_translate_coordinates(c, id, screen->root, 0, 0), nullptr);
        });
        const QPoint position = r ? QPoint(r->dst_x, r->dst_y) : QPoint(-1, -1);
        free(r);
        return position == window->contentBounds().topLeft() * 2;
    }));
    const QPoint origin = window->contentBounds().topLeft();
    auto enter = [&] {
        input.onPointerMotionAbsolute({}, origin + QPoint(100, 100));
        return until([&] {
            return pointerInWindow() == QPoint(200, 200);
        });
    };
    ASSERT_TRUE(enter());

    auto *grab = xcbReply([&] {
        return xcb_grab_pointer_reply(
                c,
                xcb_grab_pointer(c, 0, id, XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC,
                                 XCB_GRAB_MODE_ASYNC, id, XCB_NONE, XCB_CURRENT_TIME),
                nullptr);
    });
    ASSERT_TRUE(grab);
    EXPECT_EQ(grab->status, XCB_GRAB_STATUS_SUCCESS);
    free(grab);
    auto *constraints = seat->pointerConstraints();
    ASSERT_TRUE(until([&] {
        return constraints->active()
                && constraints->active()->kind() == input::PointerConstraintDelegate::Kind::Confine;
    }));
    input.onPointerMotion({}, QPointF(500, 500));
    const QRect content = window->contentBounds();
    EXPECT_NEAR(content.x() + content.width(), input.pointerPosition().x(), 0.01);
    EXPECT_NEAR(content.y() + content.height(), input.pointerPosition().y(), 0.01);
    EXPECT_LT(input.pointerPosition().x(), content.x() + content.width());
    EXPECT_LT(input.pointerPosition().y(), content.y() + content.height());
    xcb_ungrab_pointer(c, XCB_CURRENT_TIME);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return !constraints->active();
    }));

    ASSERT_TRUE(enter());
    free(xcbReply([&] {
        return xcb_xfixes_query_version_reply(c, xcb_xfixes_query_version(c, 4, 0), nullptr);
    }));
    xcb_xfixes_hide_cursor(c, id);
    xcb_warp_pointer(c, XCB_NONE, id, 0, 0, 0, 0, 50, 60);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return constraints->isLocked();
    }));
    const QPointF locked = input.pointerPosition();
    input.onPointerMotion({}, QPointF(10, 5));
    EXPECT_EQ(locked, input.pointerPosition());
    EXPECT_TRUE(until([&] {
        return pointerInWindow() == QPoint(70, 70);
    }));
    xcb_xfixes_show_cursor(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return !constraints->isLocked();
    }));
    // The lock's cursor hint puts the pointer where X11 had it.
    EXPECT_EQ(QPointF(origin) + QPointF(35, 35), input.pointerPosition());

    auto *keyboardGrab = xcbReply([&] {
        return xcb_grab_keyboard_reply(c,
                                       xcb_grab_keyboard(c, 0, id, XCB_CURRENT_TIME,
                                                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
                                       nullptr);
    });
    ASSERT_TRUE(keyboardGrab);
    EXPECT_EQ(keyboardGrab->status, XCB_GRAB_STATUS_SUCCESS);
    free(keyboardGrab);
    EXPECT_TRUE(until([&] {
        return seat->shortcutsInhibition()->isActive();
    }));
    xcb_ungrab_keyboard(c, XCB_CURRENT_TIME);
    xcb_flush(c);
    EXPECT_TRUE(until([&] {
        return !seat->shortcutsInhibition()->isActive();
    }));
    xcb_destroy_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().empty();
    }));
}

TEST_F(XwaylandTest, AuthenticationRequired)
{
    ASSERT_TRUE(start());
    xcb_auth_info_t auth {};
    char name[] = "MIT-MAGIC-COOKIE-1";
    char invalid[16] {};
    auth.name = name;
    auth.namelen = sizeof(name) - 1;
    auth.data = invalid;
    auth.datalen = sizeof(invalid);
    auto *connection = xcbReply([&] {
        return xcb_connect_to_display_with_auth_info(xwayland->display().toUtf8().constData(),
                                                     &auth, nullptr);
    });
    EXPECT_NE(xcb_connection_has_error(connection), 0);
    xcb_disconnect(connection);
    EXPECT_TRUE(xwayland->isReady());
}

// Transfer scenarios follow Sommelier's selection handlers using real protocol peers.
TEST_F(XwaylandTest, ClipboardBothDirectionsAndIncrementalTransfers)
{
    ASSERT_TRUE(start());
    ASSERT_TRUE(wayland::createDataDeviceManagerGlobal(server->display(), waylandSeat.get()));
    clipboard_test::Client native(*server);
    auto pump = [&] {
        native.pump();
    };
    ASSERT_TRUE(until([&] {
        pump();
        return native.manager && native.compositor && native.seat;
    }));
    native.initialize();
    ASSERT_TRUE(until([&] {
        pump();
        return wl_client_get_object(native.serverClient,
                                    wl_proxy_get_id(reinterpret_cast<wl_proxy *>(native.surface)))
                != nullptr;
    }));
    auto *resource = wl_client_get_object(
            native.serverClient, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(native.surface)));
    seat->setFocusedSurface(wayland::getUserDataAs<compositor::Surface>(resource));
    ASSERT_TRUE(until([&] {
        pump();
        return native.serial != 0;
    }));
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    auto window = xcb_generate_id(c);
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(c, 0, window, root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
    auto clipboard = intern(c, "CLIPBOARD");
    auto targets = intern(c, "TARGETS");
    auto utf8 = intern(c, "UTF8_STRING");
    auto incr = intern(c, "INCR");
    auto property = intern(c, "CUTefish_TEST_SELECTION");
    auto managerAtom = intern(c, "CLIPBOARD_MANAGER");
    auto saveTargets = intern(c, "SAVE_TARGETS");
    const std::string mime = "text/plain;charset=utf-8";
    for (size_t size : {size_t(0), size_t(23), size_t(65535), size_t(65536), size_t(65537),
                        size_t(2 * 1024 * 1024 + 17)}) {
        std::string payload(size, 'x');
        for (size_t i = 0; i < size; ++i)
            payload[i] = char('a' + i % 26);
        auto previous = seat->selection()->generation();
        wl_data_device_set_selection(native.devices.front(), native.source({{mime, payload}}),
                                     native.serial);
        ASSERT_TRUE(until([&] {
            pump();
            return seat->selection()->generation() != previous;
        }));
        ASSERT_TRUE(until([&] {
            pump();
            auto *r = xcbReply([&] {
                return xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, clipboard),
                                                     nullptr);
            });
            bool owned = r && r->owner && r->owner != window;
            free(r);
            return owned;
        }));
        xcb_convert_selection(c, window, clipboard, utf8, property, XCB_CURRENT_TIME);
        xcb_flush(c);
        std::string received;
        bool incremental = false, done = false, notified = false;
        auto read = [&] {
            auto *r = xcbReply([&] {
                return xcb_get_property_reply(
                        c, xcb_get_property(c, 1, window, property, XCB_ATOM_ANY, 0, 65536),
                        nullptr);
            });
            if (!r)
                return;
            if (r->type == incr)
                incremental = true;
            else if (r->type == utf8) {
                auto count = xcb_get_property_value_length(r);
                received.append(static_cast<const char *>(xcb_get_property_value(r)), count);
                done = !incremental || count == 0;
            }
            free(r);
            xcb_flush(c);
        };
        ASSERT_TRUE(until([&] {
            pump();
            while (auto *e = xcb_poll_for_event(c)) {
                auto type = e->response_type & 0x7f;
                if (type == XCB_SELECTION_NOTIFY) {
                    auto *n = reinterpret_cast<xcb_selection_notify_event_t *>(e);
                    EXPECT_EQ(n->property, property);
                    notified = true;
                    read();
                } else if (type == XCB_PROPERTY_NOTIFY && notified && incremental) {
                    auto *n = reinterpret_cast<xcb_property_notify_event_t *>(e);
                    if (n->atom == property && n->state == XCB_PROPERTY_NEW_VALUE)
                        read();
                }
                free(e);
            }
            return done;
        }));
        EXPECT_EQ(received, payload);
        EXPECT_EQ(incremental, size >= 65536);

        native.selection = nullptr;
        xcb_set_selection_owner(c, window, clipboard, XCB_CURRENT_TIME);
        xcb_flush(c);
        xcb_window_t requestor = 0;
        xcb_atom_t destination = 0;
        size_t offset = 0;
        bool importingIncr = false, saved = false;
        auto serve = [&] {
            pump();
            while (auto *e = xcb_poll_for_event(c)) {
                auto type = e->response_type & 0x7f;
                if (type == XCB_SELECTION_NOTIFY) {
                    auto *n = reinterpret_cast<xcb_selection_notify_event_t *>(e);
                    if (n->selection == managerAtom && n->target == saveTargets)
                        saved = n->property != XCB_ATOM_NONE;
                } else if (type == XCB_SELECTION_REQUEST) {
                    auto *r = reinterpret_cast<xcb_selection_request_event_t *>(e);
                    xcb_selection_notify_event_t n {};
                    n.response_type = XCB_SELECTION_NOTIFY;
                    n.requestor = r->requestor;
                    n.selection = r->selection;
                    n.target = r->target;
                    n.property = r->property;
                    n.time = r->time;
                    if (r->target == targets) {
                        xcb_atom_t offered[] = {targets, utf8};
                        xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property,
                                            XCB_ATOM_ATOM, 32, 2, offered);
                    } else if (r->target == utf8) {
                        if (size > 65536) {
                            requestor = r->requestor;
                            destination = r->property;
                            xcb_change_window_attributes(c, requestor, XCB_CW_EVENT_MASK, &mask);
                            uint32_t length = size;
                            xcb_change_property(c, XCB_PROP_MODE_REPLACE, requestor, destination,
                                                incr, 32, 1, &length);
                            importingIncr = true;
                        } else {
                            xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property,
                                                utf8, 8, payload.size(), payload.data());
                        }
                    } else
                        n.property = XCB_ATOM_NONE;
                    xcb_send_event(c, 0, r->requestor, 0, reinterpret_cast<const char *>(&n));
                } else if (type == XCB_PROPERTY_NOTIFY && importingIncr) {
                    auto *n = reinterpret_cast<xcb_property_notify_event_t *>(e);
                    if (n->window == requestor && n->atom == destination
                        && n->state == XCB_PROPERTY_DELETE) {
                        auto count = std::min(size_t(65536), payload.size() - offset);
                        xcb_change_property(c, XCB_PROP_MODE_REPLACE, requestor, destination, utf8,
                                            8, count, payload.data() + offset);
                        offset += count;
                        if (!count)
                            importingIncr = false;
                    }
                }
                free(e);
            }
            xcb_flush(c);
        };
        ASSERT_TRUE(until([&] {
            serve();
            return native.selection != nullptr;
        }));
        int fds[2];
        ASSERT_EQ(pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
        auto closeRead = qScopeGuard([&] {
            close(fds[0]);
        });
        wl_data_offer_receive(native.selection, mime.c_str(), fds[1]);
        close(fds[1]);
        received.clear();
        done = false;
        ASSERT_TRUE(until([&] {
            serve();
            char bytes[4096];
            auto count = ::read(fds[0], bytes, sizeof(bytes));
            if (count > 0)
                received.append(bytes, count);
            if (!count)
                done = true;
            return done;
        }));
        EXPECT_EQ(received, payload);
        if (size > 1024 * 1024) {
            auto *oldOffer = native.selection;
            offset = 0;
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, property, XCB_ATOM_ATOM, 32, 1,
                                &utf8);
            xcb_convert_selection(c, window, managerAtom, saveTargets, property, XCB_CURRENT_TIME);
            xcb_flush(c);
            ASSERT_TRUE(until([&] {
                serve();
                return saved && native.selection != oldOffer;
            }));
            xcb_destroy_window(c, window);
            xcb_flush(c);
            int cached[2];
            ASSERT_EQ(pipe2(cached, O_CLOEXEC | O_NONBLOCK), 0);
            auto closeCached = qScopeGuard([&] {
                close(cached[0]);
            });
            wl_data_offer_receive(native.selection, mime.c_str(), cached[1]);
            close(cached[1]);
            received.clear();
            done = false;
            ASSERT_TRUE(until([&] {
                native.pump();
                char bytes[8192];
                auto n = ::read(cached[0], bytes, sizeof(bytes));
                if (n > 0)
                    received.append(bytes, n);
                if (!n)
                    done = true;
                return done;
            }));
            EXPECT_EQ(received, payload);
        }
    }
}

TEST_F(XwaylandTest, ClipboardManagerRejectsFailedConversion)
{
    ASSERT_TRUE(start());
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    const auto owner = xcb_generate_id(c);
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    xcb_create_window(c, 0, owner, root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, 0, nullptr);
    const auto clipboard = intern(c, "CLIPBOARD"), targets = intern(c, "TARGETS"),
               utf8 = intern(c, "UTF8_STRING"), managerAtom = intern(c, "CLIPBOARD_MANAGER"),
               save = intern(c, "SAVE_TARGETS"), property = intern(c, "TEST_SAVE"),
               textAtom = intern(c, "TEXT");
    auto offeredTarget = utf8;
    xcb_set_selection_owner(c, owner, clipboard, XCB_CURRENT_TIME);
    xcb_flush(c);
    bool refused = false, completed = false;
    auto pump = [&] {
        while (auto *event = xcb_poll_for_event(c)) {
            if ((event->response_type & 0x7f) == XCB_SELECTION_REQUEST) {
                auto *request = reinterpret_cast<xcb_selection_request_event_t *>(event);
                xcb_selection_notify_event_t response {};
                response.response_type = XCB_SELECTION_NOTIFY;
                response.requestor = request->requestor;
                response.selection = request->selection;
                response.target = request->target;
                response.time = request->time;
                if (request->target == targets) {
                    response.property = request->property;
                    xcb_change_property(c, XCB_PROP_MODE_REPLACE, request->requestor,
                                        request->property, XCB_ATOM_ATOM, 32, 1, &offeredTarget);
                } else if (request->target == textAtom) {
                    response.property = request->property;
                    const unsigned char text[] = {'c', 'a', 'f', 0xe9};
                    xcb_change_property(c, XCB_PROP_MODE_REPLACE, request->requestor,
                                        request->property, XCB_ATOM_STRING, 8, sizeof(text), text);
                } else
                    refused = true;
                xcb_send_event(c, 0, request->requestor, 0, reinterpret_cast<char *>(&response));
            } else if ((event->response_type & 0x7f) == XCB_SELECTION_NOTIFY) {
                auto *response = reinterpret_cast<xcb_selection_notify_event_t *>(event);
                if (response->selection == managerAtom && response->target == save) {
                    EXPECT_EQ(response->property, xcb_atom_t(XCB_ATOM_NONE));
                    completed = true;
                }
            }
            free(event);
        }
        xcb_flush(c);
    };
    ASSERT_TRUE(until([&] {
        pump();
        return bool(seat->selection()->source());
    }));
    const auto original = seat->selection()->source();
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, owner, property, XCB_ATOM_ATOM, 32, 1, &utf8);
    xcb_convert_selection(c, owner, managerAtom, save, property, XCB_CURRENT_TIME);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        pump();
        return completed;
    }));
    EXPECT_TRUE(refused);
    EXPECT_EQ(seat->selection()->source(), original);
    offeredTarget = textAtom;
    xcb_set_selection_owner(c, owner, clipboard, XCB_CURRENT_TIME);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        pump();
        return seat->selection()->source() && seat->selection()->source() != original;
    }));
    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
    auto closeRead = qScopeGuard([&] {
        close(fds[0]);
    });
    seat->selection()->source()->send("text/plain;charset=utf-8", fds[1]);
    QByteArray received;
    ASSERT_TRUE(until([&] {
        pump();
        char bytes[32];
        auto count = read(fds[0], bytes, sizeof(bytes));
        if (count > 0)
            received.append(bytes, count);
        return count == 0;
    }));
    EXPECT_EQ(received, QString::fromUtf8("café").toUtf8());
}

TEST_F(XwaylandTest, QtClipboardAcrossProtocols)
{
    wayland::WaylandXdgShell xdg(compositor.get(), manager.get(), seat.get(),
                                 server->serialTracker());
    ASSERT_TRUE(wayland::createXdgShellGlobal(server->display(), &xdg));
    ASSERT_TRUE(wayland::createXdgDecorationGlobal(server->display()));
    ASSERT_TRUE(wayland::createDataDeviceManagerGlobal(server->display(), waylandSeat.get()));
    ASSERT_TRUE(wayland::createPrimarySelectionGlobal(server->display(), waylandSeat.get()));
    ASSERT_TRUE(server->addSocket(QStringLiteral("xwayland-clipboard-test")));
    ASSERT_TRUE(start());
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *probe = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(probe), 0);
    auto closeProbe = qScopeGuard([&] {
        xcb_disconnect(probe);
    });
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(probe)).data->root;
    const auto activeAtom = intern(probe, "_NET_ACTIVE_WINDOW");
    auto xFocusReady = [&] {
        return until([&] {
            auto *active = xcbReply([&] {
                return xcb_get_property_reply(
                        probe, xcb_get_property(probe, 0, root, activeAtom, XCB_ATOM_WINDOW, 0, 1),
                        nullptr);
            });
            auto *focus = xcbReply([&] {
                return xcb_get_input_focus_reply(probe, xcb_get_input_focus(probe), nullptr);
            });
            const bool ready = active && active->format == 32 && active->value_len == 1 && focus
                    && focus->focus != XCB_NONE
                    && focus->focus == *static_cast<uint32_t *>(xcb_get_property_value(active));
            free(active);
            free(focus);
            return ready;
        });
    };
    auto window = [&](const QString &title) -> wm::Window * {
        for (auto *w : manager->windows()) {
            if (w->title() == title)
                return w;
        }
        return nullptr;
    };
    for (bool x11Source : {false, true}) {
        for (const auto *format : {"text", "binary", "urls", "primary"}) {
            SCOPED_TRACE(format);
            SCOPED_TRACE(x11Source);
            QProcess source, destination;
            auto cleanup = qScopeGuard([&] {
                for (auto *process : {&source, &destination}) {
                    if (process->state() != QProcess::NotRunning) {
                        process->kill();
                        process->waitForFinished(2000);
                    }
                }
                until([&] {
                    return manager->windows().empty();
                });
            });
            auto launch = [&](QProcess &process, bool x11, const QString &title) {
                auto env = QProcessEnvironment::systemEnvironment();
                env.remove("QT_QPA_PLATFORMTHEME");
                env.insert("QT_QPA_PLATFORM", x11 ? "xcb" : "wayland");
                env.insert("DISPLAY", xwayland->display());
                env.insert("XAUTHORITY", xwayland->authority());
                env.insert("WAYLAND_DISPLAY", server->socketName());
                process.setProcessEnvironment(env);
                process.start(QStringLiteral(QT_CLIPBOARD_CLIENT),
                              {title, QString::fromLatin1(format)});
            };
            launch(source, x11Source, "clipboard-source");
            ASSERT_TRUE(until([&] {
                return window("clipboard-source");
            })) << source.errorString().toStdString()
                << " exit " << source.exitCode() << source.readAllStandardError().constData();
            window("clipboard-source")->activate();
            QByteArray activationOutput;
            ASSERT_TRUE(until([&] {
                activationOutput += source.readAllStandardOutput();
                return activationOutput.contains("FOCUSED");
            }));
            if (x11Source)
                ASSERT_TRUE(xFocusReady());
            if (x11Source && QString::fromLatin1(format) == "text") {
                seat->onKey({}, KEY_D, true);
                seat->onKey({}, KEY_D, false);
                ASSERT_TRUE(until([&] {
                    return window("clipboard-dialog");
                }));
                EXPECT_EQ(window("clipboard-dialog")->transientParent(),
                          window("clipboard-source"));
                QByteArray dialogOutput;
                ASSERT_TRUE(until([&] {
                    dialogOutput += source.readAllStandardOutput();
                    return dialogOutput.contains("ACTIVE clipboard-dialog");
                }));
                ASSERT_TRUE(xFocusReady());
                seat->onKey({}, KEY_ESC, true);
                seat->onKey({}, KEY_ESC, false);
                ASSERT_TRUE(until([&] {
                    return !window("clipboard-dialog");
                })) << source.readAllStandardError().constData();
                window("clipboard-source")->activate();
                dialogOutput.clear();
                ASSERT_TRUE(until([&] {
                    dialogOutput += source.readAllStandardOutput();
                    return dialogOutput.contains("ACTIVE clipboard-source");
                }));
                seat->onKey({}, KEY_M, true);
                seat->onKey({}, KEY_M, false);
                QByteArray menuOutput;
                ASSERT_TRUE(until([&] {
                    menuOutput += source.readAllStandardOutput();
                    return menuOutput.contains("MENU");
                }));
                EXPECT_EQ(manager->windows().size(), 1u);
                seat->onKey({}, KEY_ESC, true);
                seat->onKey({}, KEY_ESC, false);
                ASSERT_TRUE(until([&] {
                    menuOutput += source.readAllStandardOutput();
                    return menuOutput.contains("MENU_CLOSED");
                }));
                EXPECT_EQ(seat->focusedSurface(), window("clipboard-source")->rootSurface());
                EXPECT_TRUE(window("clipboard-source")->isActive());
            }
            seat->onKey({}, KEY_C, true);
            seat->onKey({}, KEY_C, false);
            QByteArray sourceOutput;
            ASSERT_TRUE(until([&] {
                sourceOutput += source.readAllStandardOutput();
                return sourceOutput.contains("PUBLISHED")
                        && (QString::fromLatin1(format) == "primary"
                                    ? seat->primarySelection()->source()
                                    : seat->selection()->source());
            })) << source.readAllStandardError().constData();
            launch(destination, !x11Source, "clipboard-destination");
            ASSERT_TRUE(until([&] {
                return window("clipboard-destination");
            })) << destination.readAllStandardError().constData();
            window("clipboard-destination")->activate();
            QByteArray destinationOutput;
            ASSERT_TRUE(until([&] {
                destinationOutput += destination.readAllStandardOutput();
                return destinationOutput.contains("FOCUSED");
            }));
            if (!x11Source)
                ASSERT_TRUE(xFocusReady());
            seat->onKey({}, KEY_V, true);
            seat->onKey({}, KEY_V, false);
            ASSERT_TRUE(until([&] {
                return destination.state() == QProcess::NotRunning;
            }));
            EXPECT_EQ(destination.exitCode(), 0) << destination.readAllStandardError().constData();
            destinationOutput += destination.readAllStandardOutput();
            EXPECT_TRUE(destinationOutput.contains("PASS"));
        }
    }
    wl_display_destroy_clients(server->display());
}

TEST_F(XwaylandTest, RealWindowMetadataConfigureAndClose)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto connection = std::async(std::launch::async, [display = xwayland->display().toStdString()] {
        return xcb_connect(display.c_str(), nullptr);
    });
    ASSERT_TRUE(until([&] {
        return connection.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }));
    auto *c = connection.get();
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    auto id = xcb_generate_id(c);
    uint32_t values[] = {screen->white_pixel,
                         XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 20, 30, 320, 200, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const char title[] = "Cutefish X11 test";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        sizeof(title) - 1, title);
    const char clazz[] = "test\0CutefishTest\0";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof(clazz) - 1, clazz);
    const auto protocols = intern(c, "WM_PROTOCOLS");
    const auto deleteWindow = intern(c, "WM_DELETE_WINDOW");
    const auto takeFocus = intern(c, "WM_TAKE_FOCUS");
    const xcb_atom_t supportedProtocols[] = {deleteWindow, takeFocus};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, protocols, XCB_ATOM_ATOM, 32, 2,
                        supportedProtocols);
    // A client can name any process here; the reported pid never comes from it.
    const uint32_t claimedPid = 1;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, intern(c, "_NET_WM_PID"),
                        XCB_ATOM_CARDINAL, 32, 1, &claimedPid);
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    })) << xwayland->error().toStdString();
    auto *window = manager->windows()[0];
    EXPECT_EQ(window->title(), title);
    EXPECT_EQ(window->applicationId(), "cutefishtest");
    // This client shares the compositor's process, which is never reported.
    EXPECT_EQ(window->processId(), 0);
    EXPECT_TRUE(window->rootSurface()->hasContents());
    EXPECT_TRUE(window->isDecorated());
    bool receivedFocus = false;
    ASSERT_TRUE(until([&] {
        while (auto *event = xcb_poll_for_event(c)) {
            if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
                if (message->type == protocols && message->data.data32[0] == takeFocus) {
                    EXPECT_NE(message->data.data32[1], uint32_t(XCB_CURRENT_TIME));
                    receivedFocus = true;
                }
            }
            free(event);
        }
        return receivedFocus;
    }));
    window->requestBounds(QRect(100, 120, 420, 300));
    // The WM and test client use separate X connections without request ordering.
    ASSERT_TRUE(until([&] {
        auto *geometry = xcbReply([&] {
            return xcb_get_geometry_reply(c, xcb_get_geometry(c, id), nullptr);
        });
        if (!geometry)
            return false;
        const bool matches = geometry->x == window->contentBounds().x()
                && geometry->width == window->contentBounds().width();
        free(geometry);
        return matches;
    }));
    auto *focus = xcbReply([&] {
        return xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
    });
    ASSERT_NE(focus, nullptr);
    EXPECT_EQ(focus->focus, id);
    free(focus);
    auto native = compositor->createSurface();
    auto *nativeWindow = manager->createWindow(native.get());
    nativeWindow->activate();
    EXPECT_TRUE(until([&] {
        auto *reply = xcbReply([&] {
            return xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
        });
        const bool done = reply && reply->focus == XCB_NONE;
        free(reply);
        return done;
    }));
    window->activate();
    manager->destroyWindow(nativeWindow);
    const auto stateAtom = intern(c, "_NET_WM_STATE");
    const auto fullscreenAtom = intern(c, "_NET_WM_STATE_FULLSCREEN");
    xcb_client_message_event_t stateMessage {};
    stateMessage.response_type = XCB_CLIENT_MESSAGE;
    stateMessage.window = id;
    stateMessage.type = stateAtom;
    stateMessage.format = 32;
    stateMessage.data.data32[0] = 1;
    stateMessage.data.data32[1] = fullscreenAtom;
    xcb_send_event(c, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&stateMessage));
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return window->stateType() == wm::WindowStateType::Fullscreen;
    }));
    stateMessage.data.data32[0] = 0;
    xcb_send_event(c, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&stateMessage));
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return window->stateType() == wm::WindowStateType::Normal;
    }));
    const uint32_t newSize[] = {400, 240};
    xcb_configure_window(c, id, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, newSize);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return window->contentBounds().size() == QSize(400, 240);
    }));
    window->maximize();
    EXPECT_EQ(window->stateType(), wm::WindowStateType::Maximized);
    window->minimize();
    EXPECT_EQ(manager->windows().size(), 1u);
    window->restore();
    window->setFullscreen(true);
    EXPECT_EQ(window->stateType(), wm::WindowStateType::Fullscreen);
    window->setFullscreen(false);
    const auto netName = intern(c, "_NET_WM_NAME");
    const auto utf8 = intern(c, "UTF8_STRING");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, netName, utf8, 8, 9, "Preferred");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 6,
                        "Legacy");
    xcb_flush(c);
    EXPECT_TRUE(until([&] {
        return window->title() == "Preferred";
    }));
    xcb_delete_property(c, id, netName);
    xcb_flush(c);
    EXPECT_TRUE(until([&] {
        return window->title() == "Legacy";
    }));
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 7,
                        "Changed");
    xcb_flush(c);
    EXPECT_TRUE(until([&] {
        return window->title() == "Changed";
    }));
    window->activate();
    const auto focusedAtom = intern(c, "_NET_WM_STATE_FOCUSED");
    auto hasState = [&](xcb_atom_t state) {
        auto *r = xcbReply([&] {
            return xcb_get_property_reply(
                    c, xcb_get_property(c, 0, id, stateAtom, XCB_ATOM_ATOM, 0, 128), nullptr);
        });
        bool found = false;
        if (r && r->format == 32) {
            auto *atoms = static_cast<xcb_atom_t *>(xcb_get_property_value(r));
            found = std::find(atoms, atoms + r->value_len, state) != atoms + r->value_len;
        }
        free(r);
        return found;
    };
    ASSERT_TRUE(until([&] {
        return hasState(focusedAtom);
    }));
    window->minimize();
    ASSERT_TRUE(until([&] {
        return !hasState(focusedAtom);
    }));
    const uint32_t urgentHints[] = {1u << 8, 1, 1, 0, 0, 0, 0, 0, 0};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32,
                        std::size(urgentHints), urgentHints);
    xcb_flush(c);
    const auto attentionAtom = intern(c, "_NET_WM_STATE_DEMANDS_ATTENTION");
    ASSERT_TRUE(until([&] {
        return hasState(attentionAtom);
    }));
    window->restore();
    window->activate();
    ASSERT_TRUE(until([&] {
        return !hasState(attentionAtom) && hasState(focusedAtom);
    }));

    const xcb_rectangle_t shape[] = {{0, 0, 50, 50}, {50, 50, 50, 50}};
    xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED, id,
                         0, 0, std::size(shape), shape);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return window->rootSurface()->node()->hasContentShape();
    }));
    EXPECT_TRUE(window->rootSurface()->hitTestRegion().contains(QPoint(20, 20)));
    EXPECT_FALSE(window->rootSurface()->hitTestRegion().contains(QPoint(75, 20)));
    EXPECT_FALSE(window->rootSurface()->node()->isOpaque());
    xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED, id,
                         0, 0, 0, nullptr);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return window->rootSurface()->hitTestRegion().isEmpty();
    }));
    xcb_shape_mask(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, id, 0, 0, XCB_NONE);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return !window->rootSurface()->node()->hasContentShape();
    }));
    xcb_unmap_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().empty();
    }));
    xcb_map_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    wayland::WaylandCutefishWindowManagement management(manager.get(), privileged.get());
    auto *managementGlobal =
            wayland::createCutefishWindowManagementGlobal(server->display(), &management);
    int shellSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, shellSockets), 0);
    auto *shellServer = privileged->adoptShellConnection(shellSockets[0]);
    auto *shellClient = wl_display_connect_to_fd(shellSockets[1]);
    auto *registry = wl_display_get_registry(shellClient);

    struct Task {
        cutefish_window_management_v1 *manager = nullptr;
        cutefish_window_v1 *window = nullptr;
        QString title, app;
        bool minimized = false, closed = false;
        int announcements = 0;
        quint64 parentId = 0;
        int parentCount = 0;
    } task;

    static const cutefish_window_v1_listener taskListener = {
            [](void *p, auto *, const char *title) {
                static_cast<Task *>(p)->title = title;
            },
            [](void *p, auto *, const char *app) {
                static_cast<Task *>(p)->app = app;
            },
            [](void *, auto *, uint32_t) {},
            [](void *p, auto *, uint32_t, uint32_t minimized, uint32_t, uint32_t, uint32_t) {
                static_cast<Task *>(p)->minimized = minimized;
            },
            [](void *, auto *, int32_t, int32_t, int32_t, int32_t) {},
            [](void *, auto *) {},
            [](void *p, auto *) {
                static_cast<Task *>(p)->closed = true;
            },
            [](void *, auto *, const char *, const char *) {},
            [](void *, auto *, int32_t fd, uint32_t, uint32_t) {
                close(fd);
            },
            [](void *, auto *) {},
            [](void *p, auto *, uint32_t high, uint32_t low) {
                auto *task = static_cast<Task *>(p);
                task->parentId = (quint64(high) << 32) | low;
                ++task->parentCount;
            },
            [](void *, auto *, uint32_t) {},
            [](void *, auto *, uint32_t) {},
    };
    static const cutefish_window_management_v1_listener managementListener = {
            [](void *p, auto *, cutefish_window_v1 *window, uint32_t, uint32_t) {
                auto *task = static_cast<Task *>(p);
                task->window = window;
                ++task->announcements;
                cutefish_window_v1_add_listener(window, &taskListener, task);
            },
            [](void *, auto *, uint32_t, uint32_t) {},
            [](void *, auto *) {},
            [](void *, auto *, uint32_t, uint32_t, uint32_t, uint32_t) {},
            [](void *, auto *, uint32_t) {},
            [](void *, auto *, uint32_t) {},
            [](void *, auto *, uint32_t, const char *) {},
            [](void *, auto *, uint32_t) {},
    };
    static const wl_registry_listener registryListener = {
            [](void *p, wl_registry *registry, uint32_t id, const char *name, uint32_t) {
                if (strcmp(name, "cutefish_window_management_v1") != 0)
                    return;
                auto *task = static_cast<Task *>(p);
                task->manager = static_cast<cutefish_window_management_v1 *>(wl_registry_bind(
                        registry, id, &cutefish_window_management_v1_interface, 1));
                cutefish_window_management_v1_add_listener(task->manager, &managementListener,
                                                           task);
            },
            [](void *, auto *, uint32_t) {},
    };
    wl_registry_add_listener(registry, &registryListener, &task);
    auto shellSync = [&] {
        return xcbReply([&] {
            return wl_display_roundtrip(shellClient);
        });
    };
    ASSERT_GE(shellSync(), 0);
    ASSERT_GE(shellSync(), 0);
    ASSERT_NE(task.window, nullptr);
    EXPECT_EQ(task.announcements, 1);
    EXPECT_EQ(task.parentId, 0u);
    EXPECT_GT(task.parentCount, 0);
    EXPECT_EQ(task.title, "Changed");
    EXPECT_EQ(task.app, "cutefishtest");
    cutefish_window_v1_minimize(task.window);
    ASSERT_GE(shellSync(), 0);
    EXPECT_TRUE(task.minimized);
    EXPECT_FALSE(task.closed);
    EXPECT_EQ(manager->windows().size(), 1u);
    cutefish_window_v1_restore(task.window);
    ASSERT_GE(shellSync(), 0);
    EXPECT_FALSE(task.minimized);
    cutefish_window_v1_close(task.window);
    ASSERT_GE(shellSync(), 0);
    bool closed = false;
    ASSERT_TRUE(until([&] {
        while (auto *event = xcb_poll_for_event(c)) {
            if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
                closed |= message->type == protocols && message->format == 32
                        && message->data.data32[0] == deleteWindow;
            }
            free(event);
        }
        return closed;
    }));
    xcb_destroy_window(c, id);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().empty();
    }));
    ASSERT_GE(shellSync(), 0);
    EXPECT_TRUE(task.closed);
    cutefish_window_v1_destroy(task.window);
    cutefish_window_management_v1_destroy(task.manager);
    wl_registry_destroy(registry);
    wl_display_disconnect(shellClient);
    wl_client_destroy(shellServer);
    privileged->removePrivateGlobal(managementGlobal);
    wl_global_destroy(managementGlobal);
    xcb_disconnect(c);
}

TEST_F(XwaylandTest, ClientLeaderLegacyHintsAndMappingOrder)
{
    ASSERT_TRUE(start());
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto leader = xcb_generate_id(c), first = xcb_generate_id(c), second = xcb_generate_id(c);
    for (auto id : {leader, first, second}) {
        uint32_t pixel = screen->white_pixel;
        xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 240, 180, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL,
                          &pixel);
    }
    const auto leaderAtom = intern(c, "WM_CLIENT_LEADER");
    const char clazz[] = "group\0LeaderClass\0";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, leader, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof(clazz) - 1, clazz);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, first, leaderAtom, XCB_ATOM_WINDOW, 32, 1,
                        &leader);
    uint32_t hints[15] {};
    hints[0] = 1 << 4;
    hints[5] = 120;
    hints[6] = 90;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, first, XCB_ATOM_WM_NORMAL_HINTS,
                        XCB_ATOM_WM_SIZE_HINTS, 32, 15, hints);
    xcb_map_window(c, second);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    xcb_map_window(c, first);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 2
                && std::any_of(manager->windows().begin(), manager->windows().end(), [](auto *w) {
                       return w->applicationId() == "leaderclass";
                   });
    }));
    wm::Window *grouped = nullptr;
    for (auto *w : manager->windows()) {
        if (w->applicationId() == "leaderclass")
            grouped = w;
    }
    ASSERT_TRUE(grouped);
    EXPECT_EQ(grouped->minimumSize(), QSize(120, 90));
    const auto listAtom = intern(c, "_NET_CLIENT_LIST");
    ASSERT_TRUE(until([&] {
        auto *r = xcbReply([&] {
            return xcb_get_property_reply(
                    c, xcb_get_property(c, 0, screen->root, listAtom, XCB_ATOM_WINDOW, 0, 10),
                    nullptr);
        });
        bool matches = r && r->format == 32 && r->value_len == 2;
        if (matches) {
            auto *ids = static_cast<uint32_t *>(xcb_get_property_value(r));
            matches = ids[0] == second && ids[1] == first;
        }
        free(r);
        return matches;
    }));
    const char updated[] = "group\0UpdatedClass\0";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, leader, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof(updated) - 1, updated);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return grouped->applicationId() == "updatedclass";
    }));
}

TEST_F(XwaylandTest, ATransientMappedModalIsModalBeforeItIsActivated)
{
    ASSERT_TRUE(start());
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto parent = xcb_generate_id(c), dialog = xcb_generate_id(c);
    for (auto id : {parent, dialog}) {
        uint32_t pixel = screen->white_pixel;
        xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, id == parent ? 400 : 160,
                          id == parent ? 300 : 120, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, XCB_CW_BACK_PIXEL, &pixel);
    }
    xcb_map_window(c, parent);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    wm::Window *parentWindow = manager->windows().front();

    std::optional<bool> modalWhenActivated;
    QObject::connect(manager.get(), &wm::WindowManager::activeWindowChanged, manager.get(),
                     [&](wm::Window *window) {
                         if (window && window != parentWindow && !modalWhenActivated)
                             modalWhenActivated = window->isModal();
                     });
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_TRANSIENT_FOR,
                        XCB_ATOM_WINDOW, 32, 1, &parent);
    const auto modalAtom = intern(c, "_NET_WM_STATE_MODAL");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, intern(c, "_NET_WM_STATE"), XCB_ATOM_ATOM,
                        32, 1, &modalAtom);
    xcb_map_window(c, dialog);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 2 && modalWhenActivated;
    }));

    wm::Window *dialogWindow = nullptr;
    for (auto *w : manager->windows()) {
        if (w != parentWindow)
            dialogWindow = w;
    }
    ASSERT_TRUE(dialogWindow);
    EXPECT_TRUE(*modalWhenActivated);
    EXPECT_TRUE(dialogWindow->isModal());
    EXPECT_EQ(parentWindow, dialogWindow->transientParent());
    EXPECT_EQ(dialogWindow, wm::modalTransient(parentWindow));
    manager->activation()->activate(parentWindow);
    EXPECT_EQ(dialogWindow, manager->activation()->activeWindow());
}

// The window manager read the dialog's properties when WM_TRANSIENT_FOR was
// set; the modal state comes after, just before mapping. The dialog's surface
// is made to arrive before the map-time read does, and the window must still
// wait for that read.
TEST_F(XwaylandTest, StateSetJustBeforeMappingIsNotLostToAnEarlySurface)
{
    ASSERT_TRUE(start());
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto parent = xcb_generate_id(c), dialog = xcb_generate_id(c);
    for (auto id : {parent, dialog}) {
        uint32_t pixel = screen->white_pixel;
        xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, id == parent ? 400 : 160,
                          id == parent ? 300 : 120, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, XCB_CW_BACK_PIXEL, &pixel);
    }
    xcb_map_window(c, parent);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    wm::Window *parentWindow = manager->windows().front();
    const auto modalAtom = intern(c, "_NET_WM_STATE_MODAL");
    const auto stateAtom = intern(c, "_NET_WM_STATE");

    xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, XCB_ATOM_WM_TRANSIENT_FOR,
                        XCB_ATOM_WINDOW, 32, 1, &parent);
    xcb_flush(c);
    until([] {
        return false;
    }, 200);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, dialog, stateAtom, XCB_ATOM_ATOM, 32, 1,
                        &modalAtom);
    xcb_map_window(c, dialog);
    xcb_flush(c);
    // One iteration: the window manager takes the map request and nothing after.
    QThread::msleep(100);
    QCoreApplication::processEvents();
    // Only the Wayland side runs while Xwayland maps and draws the dialog.
    auto dialogSurface = [&]() -> compositor::Surface * {
        for (auto *surface : compositor->surfaces()) {
            if (surface != parentWindow->rootSurface() && surface->hasContents())
                return surface;
        }
        return nullptr;
    };
    QDeadlineTimer deadline(5000);
    while (!dialogSurface() && !deadline.hasExpired()) {
        server->dispatch(std::chrono::milliseconds(10));
        server->flush();
    }
    ASSERT_TRUE(dialogSurface());

    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 2;
    }));
    wm::Window *dialogWindow = manager->windowForSurface(dialogSurface());
    ASSERT_TRUE(dialogWindow);
    EXPECT_TRUE(dialogWindow->isModal());
    EXPECT_EQ(parentWindow, dialogWindow->transientParent());
}

// SDL and GTK ask for fullscreen as soon as they see themselves mapped, while
// the window manager still waits for Xwayland's surface to have content. A
// window with no background has none until it draws, which holds that gap open.
TEST_F(XwaylandTest, StateRequestedBeforeTheWindowIsManagedIsKept)
{
    ASSERT_TRUE(start());
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    auto *c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(c);
    });
    auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const auto id = xcb_generate_id(c);
    const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 200, 150, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_EVENT_MASK,
                      &mask);
    const auto stateAtom = intern(c, "_NET_WM_STATE");
    const auto fullscreenAtom = intern(c, "_NET_WM_STATE_FULLSCREEN");
    xcb_map_window(c, id);
    xcb_flush(c);
    bool mapped = false;
    ASSERT_TRUE(until([&] {
        while (xcb_generic_event_t *event = xcb_poll_for_event(c)) {
            mapped |= (event->response_type & ~0x80) == XCB_MAP_NOTIFY;
            free(event);
        }
        return mapped;
    }));
    xcb_client_message_event_t message {};
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = id;
    message.type = stateAtom;
    message.data.data32[0] = 1;
    message.data.data32[1] = fullscreenAtom;
    message.data.data32[3] = 1;
    xcb_send_event(c, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char *>(&message));
    xcb_flush(c);
    until([] {
        return false;
    }, 300);
    ASSERT_TRUE(manager->windows().empty());

    const uint32_t pixel = screen->white_pixel;
    xcb_change_window_attributes(c, id, XCB_CW_BACK_PIXEL, &pixel);
    xcb_clear_area(c, 0, id, 0, 0, 0, 0);
    xcb_flush(c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    EXPECT_EQ(wm::WindowStateType::Fullscreen, manager->windows().front()->stateType());
}

TEST_F(XwaylandTest, RealSoftwareRenderingAndInput)
{
    for (const double scale : {1.0, 1.5, 2.0}) {
        SCOPED_TRACE(scale);
        output.scale = *compositor::OutputScale::fromNumerator(qRound(scale * 120));
        output.pixelSize = QSize(qRound(800 * scale), qRound(600 * scale));
        compositor->setOutputScale(output.scale);
        wayland::updateOutputGlobal(outputGlobal, &output);
        const auto devicePath = qgetenv("CUTEFISH_TEST_DRM_DEVICE");
        int drmFd = devicePath.isEmpty() ? -1 : open(devicePath.constData(), O_RDWR | O_CLOEXEC);
        gbm_device *device = drmFd >= 0 ? gbm_create_device(drmFd) : nullptr;
        auto deviceCleanup = qScopeGuard([&] {
            if (device)
                gbm_device_destroy(device);
            if (drmFd >= 0)
                close(drmFd);
        });
        ASSERT_TRUE(devicePath.isEmpty() || device);
        auto renderer = device ? render::EglRenderer::create(device, DRM_FORMAT_XRGB8888)
                               : render::EglRendererTestAccess::create();
        ASSERT_NE(renderer, nullptr);
        std::unique_ptr<gbm_surface, decltype(&gbm_surface_destroy)> gbmSurface(
                device ? gbm_surface_create(device, output.pixelSize.width(),
                                            output.pixelSize.height(), DRM_FORMAT_XRGB8888,
                                            GBM_BO_USE_RENDERING)
                       : nullptr,
                gbm_surface_destroy);
        std::unique_ptr<render::EglRenderTarget> target;
        if (device) {
            ASSERT_TRUE(gbmSurface);
            target = renderer->createTarget(gbmSurface.get(), output.pixelSize);
        } else {
            const EGLint attrs[] = {EGL_WIDTH, output.pixelSize.width(), EGL_HEIGHT,
                                    output.pixelSize.height(), EGL_NONE};
            auto surface =
                    eglCreatePbufferSurface(renderer->eglDisplay(), renderer->eglConfig(), attrs);
            ASSERT_NE(surface, EGL_NO_SURFACE);
            target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), surface,
                                                               nullptr, output.pixelSize);
        }
        ASSERT_TRUE(target);
        auto cleanup = qScopeGuard([&] {
            xwayland->stop();
            scene.root()->releaseRenderResources();
            renderer->clearTextureCache();
            renderer->releaseTarget(target.get());
        });
        ASSERT_TRUE(renderer->beginFrame(target.get()));
        ASSERT_TRUE(start());
        qputenv("XAUTHORITY", xwayland->authority().toUtf8());
        auto *c = xcbReply([&] {
            return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
        });
        ASSERT_EQ(xcb_connection_has_error(c), 0);
        auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
        const auto id = xcb_generate_id(c);
        uint32_t values[] = {screen->white_pixel,
                             XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS};
        xcb_create_window(c, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, 300, 200, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
        xcb_map_window(c, id);
        xcb_flush(c);
        ASSERT_TRUE(until([&] {
            return manager->windows().size() == 1;
        }));
        auto *window = manager->windows()[0];
        static_cast<scene::WindowAnimation *>(window->node())->finish();
        compositor->updateTextures(*renderer);
        renderer->setRasterScale(scale);
        renderer->clear({0, 0, 0, 1});
        scene.root()->render(*renderer);
        const auto center = window->contentBounds().center();
        uint8_t pixel[4] {};
        glReadPixels(qRound(center.x() * scale),
                     output.pixelSize.height() - 1 - qRound(center.y() * scale), 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixel);
        EXPECT_EQ(glGetError(), GLenum(GL_NO_ERROR));
        EXPECT_GT(pixel[0], 240);
        EXPECT_GT(pixel[1], 240);
        EXPECT_GT(pixel[2], 240);
        const xcb_rectangle_t visible[] = {{0, 0, 75, 200}};
        xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                             id, 0, 0, 1, visible);
        xcb_flush(c);
        ASSERT_TRUE(until([&] {
            return window->rootSurface()->node()->hasContentShape();
        }));
        renderer->clear({0, 0, 0, 1});
        scene.root()->render(*renderer);
        glReadPixels(qRound(center.x() * scale),
                     output.pixelSize.height() - 1 - qRound(center.y() * scale), 1, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixel);
        EXPECT_LT(pixel[0], 30);
        EXPECT_LT(pixel[1], 30);
        EXPECT_LT(pixel[2], 30);
        xcb_shape_mask(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, id, 0, 0, XCB_NONE);
        xcb_flush(c);
        ASSERT_TRUE(until([&] {
            return !window->rootSurface()->node()->hasContentShape();
        }));
        seat->onPointerMotion(std::chrono::steady_clock::now(), center);
        seat->onPointerButton(std::chrono::steady_clock::now(), center, BTN_LEFT, true);
        seat->onPointerButton(std::chrono::steady_clock::now(), center, BTN_LEFT, false);
        seat->onKey(std::chrono::steady_clock::now(), KEY_A, true);
        seat->onKey(std::chrono::steady_clock::now(), KEY_A, false);
        bool key = false, button = false;
        EXPECT_TRUE(until([&] {
            while (auto *e = xcb_poll_for_event(c)) {
                key |= (e->response_type & 0x7f) == XCB_KEY_PRESS;
                button |= (e->response_type & 0x7f) == XCB_BUTTON_PRESS;
                free(e);
            }
            return key && button;
        }));
        const auto popup = xcb_generate_id(c);
        const bool overrideRedirect = scale != 1.5;
        uint32_t popupValues[] = {0, uint32_t(overrideRedirect)};
        auto popupRect = QRect(window->contentBounds().topLeft() + QPoint(20, 20), QSize(80, 60));
        xcb_create_window(c, XCB_COPY_FROM_PARENT, popup, screen->root, popupRect.x(),
                          popupRect.y(), 80, 60, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT,
                          popupValues);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, popup, XCB_ATOM_WM_TRANSIENT_FOR,
                            XCB_ATOM_WINDOW, 32, 1, &id);
        const auto popupType = intern(c, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, popup, intern(c, "_NET_WM_WINDOW_TYPE"),
                            XCB_ATOM_ATOM, 32, 1, &popupType);
        xcb_map_window(c, popup);
        xcb_flush(c);
        compositor::Surface *popupSurface = nullptr;
        ASSERT_TRUE(until([&] {
            QPointF local;
            auto *hit = input::findTargetSurface(compositor.get(), popupRect.center(), &local);
            if (hit && hit != window->rootSurface())
                popupSurface = hit;
            return popupSurface != nullptr;
        }));
        EXPECT_EQ(manager->windows().size(), 1u);
        EXPECT_EQ(manager->windowForSurface(popupSurface), window);
        EXPECT_EQ(popupSurface->position(), popupRect.topLeft());
        window->minimize();
        EXPECT_TRUE(popupSurface->isHidden());
        window->restore();
        EXPECT_FALSE(popupSurface->isHidden());
        xcb_destroy_window(c, id);
        xcb_flush(c);
        ASSERT_TRUE(until([&] {
            return manager->windows().empty();
        }));
        EXPECT_EQ(manager->windowForSurface(popupSurface), nullptr);
        xcb_destroy_window(c, popup);
        xcb_flush(c);
        xcb_disconnect(c);
    }
}

// A bare XDND peer on its own X11 connection. It records the Xdnd messages it
// receives, serves XdndSelection as text/plain and reads back a conversion.
struct XdndPeer {
    xcb_connection_t *c = nullptr;
    xcb_window_t root = 0;
    xcb_window_t window = 0;
    std::map<std::string, xcb_atom_t> atoms;
    std::vector<xcb_client_message_event_t> messages;
    std::string payload;
    std::string received;
    bool converted = false;
    xcb_timestamp_t pressTime = 0;
    bool pressed = false;

    xcb_atom_t operator[](const char *name)
    {
        auto &atom = atoms[name];
        if (!atom)
            atom = intern(c, name);
        return atom;
    }

    void create(const QRect &rect)
    {
        auto *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
        root = screen->root;
        window = xcb_generate_id(c);
        // A background gives Xwayland a buffer to commit, which is what maps it.
        const uint32_t values[] = {screen->white_pixel,
                                   XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_PROPERTY_CHANGE};
        xcb_create_window(c, XCB_COPY_FROM_PARENT, window, root, rect.x(), rect.y(), rect.width(),
                          rect.height(), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
        const uint32_t version = 5;
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, (*this)["XdndAware"], XCB_ATOM_ATOM,
                            32, 1, &version);
        xcb_map_window(c, window);
        xcb_flush(c);
    }

    void send(xcb_window_t to, const char *type, std::array<uint32_t, 5> data)
    {
        xcb_client_message_event_t e {};
        e.response_type = XCB_CLIENT_MESSAGE;
        e.format = 32;
        e.window = to;
        e.type = (*this)[type];
        std::copy(data.begin(), data.end(), e.data.data32);
        xcb_send_event(c, 0, to, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&e));
        xcb_flush(c);
    }

    const xcb_client_message_event_t *last(const char *type)
    {
        const xcb_atom_t atom = (*this)[type];
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->type == atom)
                return &*it;
        }
        return nullptr;
    }

    void poll()
    {
        while (auto *e = xcb_poll_for_event(c)) {
            const auto type = e->response_type & 0x7f;
            if (type == XCB_CLIENT_MESSAGE) {
                messages.push_back(*reinterpret_cast<xcb_client_message_event_t *>(e));
            } else if (type == XCB_BUTTON_PRESS) {
                pressTime = reinterpret_cast<xcb_button_press_event_t *>(e)->time;
                pressed = true;
            } else if (type == XCB_SELECTION_REQUEST) {
                auto *r = reinterpret_cast<xcb_selection_request_event_t *>(e);
                xcb_selection_notify_event_t n {};
                n.response_type = XCB_SELECTION_NOTIFY;
                n.requestor = r->requestor;
                n.selection = r->selection;
                n.target = r->target;
                n.property = r->property;
                n.time = r->time;
                if (r->target == (*this)["text/plain"])
                    xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property,
                                        r->target, 8, payload.size(), payload.data());
                else
                    n.property = XCB_ATOM_NONE;
                xcb_send_event(c, 0, r->requestor, 0, reinterpret_cast<const char *>(&n));
            } else if (type == XCB_SELECTION_NOTIFY) {
                auto *n = reinterpret_cast<xcb_selection_notify_event_t *>(e);
                if (n->property) {
                    auto *r = xcb_get_property_reply(
                            c, xcb_get_property(c, 1, window, n->property, XCB_ATOM_ANY, 0, 65536),
                            nullptr);
                    if (r)
                        received.assign(static_cast<const char *>(xcb_get_property_value(r)),
                                        xcb_get_property_value_length(r));
                    free(r);
                }
                converted = true;
            }
            free(e);
        }
        xcb_flush(c);
    }
};

// A mapped native Wayland surface for the other end of a drag.
compositor::Surface *mapNativeSurface(clipboard_test::Client &native, const QPoint &position)
{
    auto *resource = wl_client_get_object(
            native.serverClient, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(native.surface)));
    auto *surface = resource ? wayland::surfaceFromResource(resource) : nullptr;
    if (!surface)
        return nullptr;
    auto pool = testsupport::makePool(100 * 100 * 4);
    surface->attach(pool->createBuffer(QSize(100, 100), 0, 400, render::PixelFormat::Bgra8888,
                                       nullptr),
                    {});
    surface->commit();
    surface->setPosition(position);
    return surface;
}

TEST_F(XwaylandTest, WaylandDragDropsOnAnX11Window)
{
    ASSERT_TRUE(start());
    ASSERT_TRUE(wayland::createDataDeviceManagerGlobal(server->display(), waylandSeat.get()));
    clipboard_test::Client native(*server);
    ASSERT_TRUE(until([&] {
        native.pump();
        return native.manager && native.compositor && native.seat;
    }));
    native.initialize();
    ASSERT_TRUE(until([&] {
        native.pump();
        return mapNativeSurface(native, {500, 0});
    }));
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    XdndPeer x;
    x.c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(x.c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(x.c);
    });
    x.create(QRect(0, 0, 300, 200));
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    const QPoint over = manager->windows()[0]->contentBounds().center();

    seat->onPointerMotion(std::chrono::steady_clock::now(), QPointF(550, 50));
    seat->onPointerButton(std::chrono::steady_clock::now(), QPointF(550, 50), BTN_LEFT, true);
    ASSERT_TRUE(until([&] {
        native.pump();
        return native.buttonSerial != 0;
    }));
    auto *source = native.source({{"text/plain", "drag bytes"}});
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                                               | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    wl_data_device_start_drag(native.devices.front(), source, native.surface, nullptr,
                              native.buttonSerial);
    ASSERT_TRUE(until([&] {
        native.pump();
        return seat->drag()->active();
    }));

    seat->onPointerMotion(std::chrono::steady_clock::now(), over);
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return x.last("XdndPosition") != nullptr;
    }));
    const auto enter = *x.last("XdndEnter");
    const auto position = *x.last("XdndPosition");
    const xcb_window_t dragSource = enter.data.data32[0];
    EXPECT_EQ(enter.data.data32[1] >> 24, 5u);
    EXPECT_EQ(enter.data.data32[2], x["text/plain"]);
    EXPECT_EQ(position.data.data32[0], dragSource);
    EXPECT_EQ(position.data.data32[2], uint32_t(over.x() << 16 | over.y()));
    EXPECT_EQ(position.data.data32[4], x["XdndActionCopy"]);

    x.send(dragSource, "XdndStatus", {x.window, 3, 0, 0, x["XdndActionCopy"]});
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return native.sourceAction == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    }));
    seat->onPointerButton(std::chrono::steady_clock::now(), over, BTN_LEFT, false);
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return x.last("XdndDrop") != nullptr;
    }));
    EXPECT_EQ(native.performed, 1u);

    // Converted at the drop's timestamp, as XDND says; it must be a real one.
    const xcb_timestamp_t dropTime = x.last("XdndDrop")->data.data32[2];
    EXPECT_NE(dropTime, xcb_timestamp_t(XCB_CURRENT_TIME));
    EXPECT_EQ(position.data.data32[3], dropTime);
    xcb_convert_selection(x.c, x.window, x["XdndSelection"], x["text/plain"],
                          x["CUTEFISH_TEST_DROP"], dropTime);
    xcb_flush(x.c);
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return x.converted;
    }));
    EXPECT_EQ(x.received, "drag bytes");
    x.send(dragSource, "XdndFinished", {x.window, 1, x["XdndActionCopy"]});
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return native.finished == 1;
    }));
    EXPECT_EQ(native.cancelled, 0u);
    EXPECT_FALSE(seat->drag()->active());
}

TEST_F(XwaylandTest, X11DragDropsOnAWaylandSurface)
{
    ASSERT_TRUE(start());
    ASSERT_TRUE(wayland::createDataDeviceManagerGlobal(server->display(), waylandSeat.get()));
    clipboard_test::Client native(*server);
    ASSERT_TRUE(until([&] {
        native.pump();
        return native.manager && native.compositor && native.seat;
    }));
    native.initialize();
    ASSERT_TRUE(until([&] {
        native.pump();
        return mapNativeSurface(native, {500, 0});
    }));
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    XdndPeer x;
    x.c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(x.c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(x.c);
    });
    x.create(QRect(0, 0, 300, 200));
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    const QPoint inside = manager->windows()[0]->contentBounds().center();
    x.payload = "x11 bytes";

    // The press gives Xwayland the implicit grab an X11 drag runs under.
    seat->onPointerMotion(std::chrono::steady_clock::now(), inside);
    seat->onPointerButton(std::chrono::steady_clock::now(), inside, BTN_LEFT, true);
    ASSERT_TRUE(until([&] {
        x.poll();
        return x.pressed;
    }));
    xcb_set_selection_owner(x.c, x.window, x["XdndSelection"], x.pressTime);
    xcb_flush(x.c);
    const QPoint target(550, 50);
    seat->onPointerMotion(std::chrono::steady_clock::now(), target);

    // Off every X11 window, the proxy is what the X11 source finds under the pointer.
    xcb_window_t proxy = 0;
    ASSERT_TRUE(until([&] {
        x.poll();
        auto *r = xcb_translate_coordinates_reply(
                x.c, xcb_translate_coordinates(x.c, x.root, x.root, target.x(), target.y()),
                nullptr);
        if (r && r->child && r->child != x.window)
            proxy = r->child;
        free(r);
        return proxy != 0;
    }));
    x.send(proxy, "XdndEnter", {x.window, 5u << 24, x["text/plain"], x["UTF8_STRING"], 0});
    x.send(proxy, "XdndPosition",
           {x.window, 0, uint32_t(target.x() << 16 | target.y()), XCB_CURRENT_TIME,
            x["XdndActionCopy"]});
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return native.dragOffer != nullptr && x.last("XdndStatus") != nullptr;
    }));
    const auto &offered = native.formats[native.dragOffer];
    EXPECT_NE(std::find(offered.begin(), offered.end(), "text/plain"), offered.end());
    EXPECT_NE(std::find(offered.begin(), offered.end(), "text/plain;charset=utf-8"),
              offered.end());
    EXPECT_EQ(native.dragEnterPosition, QPointF(50, 50));
    EXPECT_EQ(x.last("XdndStatus")->data.data32[1] & 1, 0u);

    wl_data_offer_accept(native.dragOffer, native.dragSerial, "text/plain");
    wl_data_offer_set_actions(native.dragOffer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        auto *status = x.last("XdndStatus");
        return status && (status->data.data32[1] & 1);
    }));
    EXPECT_EQ(x.last("XdndStatus")->data.data32[0], proxy);
    EXPECT_EQ(x.last("XdndStatus")->data.data32[4], x["XdndActionCopy"]);

    seat->onPointerButton(std::chrono::steady_clock::now(), target, BTN_LEFT, false);
    x.send(proxy, "XdndDrop", {x.window, 0, XCB_CURRENT_TIME});
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return native.drops == 1;
    }));
    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
    auto closeRead = qScopeGuard([&] {
        close(fds[0]);
    });
    wl_data_offer_receive(native.dragOffer, "text/plain", fds[1]);
    close(fds[1]);
    std::string received;
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        char bytes[4096];
        const auto count = ::read(fds[0], bytes, sizeof(bytes));
        if (count > 0)
            received.append(bytes, count);
        return count == 0;
    }));
    EXPECT_EQ(received, "x11 bytes");
    wl_data_offer_finish(native.dragOffer);
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return x.last("XdndFinished") != nullptr;
    }));
    EXPECT_EQ(x.last("XdndFinished")->data.data32[1] & 1, 1u);
    EXPECT_EQ(x.last("XdndFinished")->data.data32[2], x["XdndActionCopy"]);
    EXPECT_FALSE(seat->drag()->active());
}

TEST_F(XwaylandTest, X11DragWithATypeListCanLeaveAndBeRefused)
{
    ASSERT_TRUE(start());
    ASSERT_TRUE(wayland::createDataDeviceManagerGlobal(server->display(), waylandSeat.get()));
    clipboard_test::Client native(*server);
    ASSERT_TRUE(until([&] {
        native.pump();
        return native.manager && native.compositor && native.seat;
    }));
    native.initialize();
    ASSERT_TRUE(until([&] {
        native.pump();
        return mapNativeSurface(native, {500, 0});
    }));
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    XdndPeer x;
    x.c = xcbReply([&] {
        return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
    });
    ASSERT_EQ(xcb_connection_has_error(x.c), 0);
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(x.c);
    });
    x.create(QRect(0, 0, 300, 200));
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    const QPoint inside = manager->windows()[0]->contentBounds().center();
    seat->onPointerMotion(std::chrono::steady_clock::now(), inside);
    seat->onPointerButton(std::chrono::steady_clock::now(), inside, BTN_LEFT, true);
    ASSERT_TRUE(until([&] {
        x.poll();
        return x.pressed;
    }));
    xcb_set_selection_owner(x.c, x.window, x["XdndSelection"], x.pressTime);
    // Four types do not fit XdndEnter, so they go in XdndTypeList.
    const xcb_atom_t types[] = {x["text/plain"], x["UTF8_STRING"], x["text/uri-list"],
                                x["application/x-test"]};
    xcb_change_property(x.c, XCB_PROP_MODE_REPLACE, x.window, x["XdndTypeList"], XCB_ATOM_ATOM, 32,
                        std::size(types), types);
    xcb_flush(x.c);
    const QPoint target(550, 50);
    seat->onPointerMotion(std::chrono::steady_clock::now(), target);
    xcb_window_t proxy = 0;
    ASSERT_TRUE(until([&] {
        x.poll();
        auto *r = xcb_translate_coordinates_reply(
                x.c, xcb_translate_coordinates(x.c, x.root, x.root, target.x(), target.y()),
                nullptr);
        if (r && r->child && r->child != x.window)
            proxy = r->child;
        free(r);
        return proxy != 0;
    }));
    const auto enter = [&] {
        x.send(proxy, "XdndEnter", {x.window, 5u << 24 | 1, 0, 0, 0});
        x.send(proxy, "XdndPosition",
               {x.window, 0, uint32_t(target.x() << 16 | target.y()), XCB_CURRENT_TIME,
                x["XdndActionCopy"]});
    };
    enter();
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return native.dragOffer != nullptr;
    }));
    const auto &offered = native.formats[native.dragOffer];
    EXPECT_NE(std::find(offered.begin(), offered.end(), "text/uri-list"), offered.end());
    EXPECT_NE(std::find(offered.begin(), offered.end(), "application/x-test"), offered.end());

    // XdndLeave ends the Wayland drag the enter started.
    x.send(proxy, "XdndLeave", {x.window, 0, 0, 0, 0});
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return !seat->drag()->active()
                && std::count(native.events.begin(), native.events.end(), "drag-leave") == 1;
    }));

    // A drop nothing accepted is finished as refused.
    native.dragOffer = nullptr;
    enter();
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return native.dragOffer != nullptr;
    }));
    seat->onPointerButton(std::chrono::steady_clock::now(), target, BTN_LEFT, false);
    x.send(proxy, "XdndDrop", {x.window, 0, XCB_CURRENT_TIME});
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        return x.last("XdndFinished") != nullptr;
    }));
    EXPECT_EQ(x.last("XdndFinished")->data.data32[1] & 1, 0u);
    EXPECT_EQ(native.drops, 0u);
    EXPECT_FALSE(seat->drag()->active());
    // With the button up the proxy is gone, so X11 windows find each other again.
    ASSERT_TRUE(until([&] {
        auto *r = xcb_translate_coordinates_reply(
                x.c, xcb_translate_coordinates(x.c, x.root, x.root, target.x(), target.y()),
                nullptr);
        const bool gone = r && r->child != proxy;
        free(r);
        return gone;
    }));
    // Naming the proxy in a message is not a drag without the pointer making one.
    const auto statuses = std::count_if(x.messages.begin(), x.messages.end(), [&](const auto &m) {
        return m.type == x["XdndStatus"];
    });
    enter();
    QDeadlineTimer settle(300);
    while (!settle.hasExpired()) {
        native.pump();
        x.poll();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    EXPECT_FALSE(seat->drag()->active());
    EXPECT_EQ(std::count_if(x.messages.begin(), x.messages.end(),
                            [&](const auto &m) {
                                return m.type == x["XdndStatus"];
                            }),
              statuses);
}

TEST_F(XwaylandTest, WaylandDragFollowsXdndProxyAndCancelsWhenRefused)
{
    ASSERT_TRUE(start());
    ASSERT_TRUE(wayland::createDataDeviceManagerGlobal(server->display(), waylandSeat.get()));
    clipboard_test::Client native(*server);
    ASSERT_TRUE(until([&] {
        native.pump();
        return native.manager && native.compositor && native.seat;
    }));
    native.initialize();
    ASSERT_TRUE(until([&] {
        native.pump();
        return mapNativeSurface(native, {500, 0});
    }));
    qputenv("XAUTHORITY", xwayland->authority().toUtf8());
    XdndPeer x, proxyPeer;
    for (auto *peer : {&x, &proxyPeer}) {
        peer->c = xcbReply([&] {
            return xcb_connect(xwayland->display().toUtf8().constData(), nullptr);
        });
        ASSERT_EQ(xcb_connection_has_error(peer->c), 0);
    }
    auto cleanup = qScopeGuard([&] {
        xcb_disconnect(x.c);
        xcb_disconnect(proxyPeer.c);
    });
    // The proxy is another client's unmapped window that names itself.
    proxyPeer.root = xcb_setup_roots_iterator(xcb_get_setup(proxyPeer.c)).data->root;
    proxyPeer.window = xcb_generate_id(proxyPeer.c);
    xcb_create_window(proxyPeer.c, 0, proxyPeer.window, proxyPeer.root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
    const uint32_t version = 5;
    xcb_change_property(proxyPeer.c, XCB_PROP_MODE_REPLACE, proxyPeer.window,
                        proxyPeer["XdndAware"], XCB_ATOM_ATOM, 32, 1, &version);
    xcb_change_property(proxyPeer.c, XCB_PROP_MODE_REPLACE, proxyPeer.window,
                        proxyPeer["XdndProxy"], XCB_ATOM_WINDOW, 32, 1, &proxyPeer.window);
    xcb_flush(proxyPeer.c);
    x.create(QRect(0, 0, 300, 200));
    xcb_change_property(x.c, XCB_PROP_MODE_REPLACE, x.window, x["XdndProxy"], XCB_ATOM_WINDOW, 32,
                        1, &proxyPeer.window);
    xcb_flush(x.c);
    ASSERT_TRUE(until([&] {
        return manager->windows().size() == 1;
    }));
    const QPoint over = manager->windows()[0]->contentBounds().center();

    seat->onPointerMotion(std::chrono::steady_clock::now(), QPointF(550, 50));
    seat->onPointerButton(std::chrono::steady_clock::now(), QPointF(550, 50), BTN_LEFT, true);
    ASSERT_TRUE(until([&] {
        native.pump();
        return native.buttonSerial != 0;
    }));
    auto *source = native.source({{"text/plain", "drag bytes"}});
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_start_drag(native.devices.front(), source, native.surface, nullptr,
                              native.buttonSerial);
    ASSERT_TRUE(until([&] {
        native.pump();
        return seat->drag()->active();
    }));
    seat->onPointerMotion(std::chrono::steady_clock::now(), over);
    ASSERT_TRUE(until([&] {
        native.pump();
        x.poll();
        proxyPeer.poll();
        return proxyPeer.last("XdndPosition") != nullptr;
    }));
    EXPECT_TRUE(x.messages.empty());
    // Sent to the proxy, the messages still name the window under the drag.
    EXPECT_EQ(proxyPeer.last("XdndEnter")->window, x.window);
    const xcb_window_t dragSource = proxyPeer.last("XdndEnter")->data.data32[0];

    proxyPeer.send(dragSource, "XdndStatus", {x.window, 0, 0, 0, 0});
    seat->onPointerButton(std::chrono::steady_clock::now(), over, BTN_LEFT, false);
    ASSERT_TRUE(until([&] {
        native.pump();
        proxyPeer.poll();
        return native.cancelled == 1 && proxyPeer.last("XdndLeave") != nullptr;
    }));
    EXPECT_EQ(proxyPeer.last("XdndDrop"), nullptr);
    EXPECT_EQ(native.performed, 0u);
    EXPECT_FALSE(seat->drag()->active());
}

// The Wayland drag-and-drop toolkit clients, with one side on X11.
class XwaylandDndToolkitTest : public XwaylandTest
{
protected:
    struct Child : QProcess {
        QByteArray output;

        ~Child() override
        {
            if (state() != NotRunning) {
                kill();
                waitForFinished(2000);
            }
        }

        bool saw(const char *marker)
        {
            output += readAllStandardOutput();
            return output.contains(marker);
        }
    };

    void matrix(bool sourceX11, bool sourceGtk, bool destinationGtk)
    {
        if (QStandardPaths::findExecutable("python3").isEmpty())
            GTEST_SKIP() << "python3 unavailable";
        ASSERT_TRUE(start()) << xwayland->error().toStdString();
        wayland::WaylandXdgShell xdg(compositor.get(), manager.get(), seat.get(),
                                     server->serialTracker());
        ASSERT_TRUE(wayland::createXdgShellGlobal(server->display(), &xdg));
        ASSERT_TRUE(wayland::createXdgDecorationGlobal(server->display()));
        ASSERT_TRUE(wayland::createDataDeviceManagerGlobal(server->display(), waylandSeat.get()));
        ASSERT_TRUE(server->addSocket(QStringLiteral("xwayland-dnd-test")));
        QTimer frames;
        QObject::connect(&frames, &QTimer::timeout, &frames, [&] {
            compositor->frameSubmitted();
            compositor->frameDrawn(wayland::nowInMilliseconds());
        });
        frames.start(5);
        Child source, destination;
        auto launch = [&](Child &process, bool x11, bool gtk, const QString &title) {
            auto env = QProcessEnvironment::systemEnvironment();
            for (const auto *name : {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                     "QT_QPA_PLATFORMTHEME", "QT_QUICK_BACKEND", "LD_PRELOAD",
                                     "WAYLAND_SOCKET"})
                env.remove(QString::fromLatin1(name));
            env.insert("XDG_RUNTIME_DIR", runtime.path());
            if (x11) {
                env.insert("DISPLAY", xwayland->display());
                env.insert("XAUTHORITY", xwayland->authority());
                env.insert("QT_QPA_PLATFORM", "xcb");
                env.insert("GDK_BACKEND", "x11");
                env.insert("WAYLAND_DISPLAY", "nonexistent-native-display");
            } else {
                env.insert("QT_QPA_PLATFORM", "wayland");
                env.insert("GDK_BACKEND", "wayland");
                env.insert("WAYLAND_DISPLAY", server->socketName());
            }
            process.setProcessEnvironment(env);
            process.start(gtk ? QStringLiteral("/usr/bin/python3") : QStringLiteral(QT_DND_CLIENT),
                          gtk ? QStringList {QStringLiteral(GTK_DND_CLIENT), title}
                              : QStringList {title});
        };
        auto window = [&](const QString &title) -> wm::Window * {
            for (auto *w : manager->windows()) {
                if (w->title() == title)
                    return w;
            }
            return nullptr;
        };
        launch(source, sourceX11, sourceGtk, QStringLiteral("dnd-source"));
        ASSERT_TRUE(until([&] {
            return window("dnd-source") != nullptr;
        }))
                << source.readAllStandardError().constData();
        launch(destination, !sourceX11, destinationGtk, QStringLiteral("dnd-destination"));
        ASSERT_TRUE(until([&] {
            return window("dnd-destination") != nullptr;
        }))
                << destination.readAllStandardError().constData();
        auto *sourceWindow = window("dnd-source");
        auto *destinationWindow = window("dnd-destination");
        sourceWindow->setBounds(QRect(QPoint(20, 60), sourceWindow->bounds().size()));
        destinationWindow->setBounds(QRect(QPoint(400, 60), destinationWindow->bounds().size()));
        ASSERT_TRUE(until([&] {
            return sourceWindow->rootSurface()->isMapped()
                    && destinationWindow->rootSurface()->isMapped()
                    && sourceWindow->surfaceNode()->absolutePosition()
                    == sourceWindow->contentBounds().topLeft() + sourceWindow->surfaceOrigin()
                    && destinationWindow->surfaceNode()->absolutePosition()
                    == destinationWindow->contentBounds().topLeft()
                            + destinationWindow->surfaceOrigin();
        }));
        // processEvents returns as soon as nothing is pending; the toolkits and
        // Xwayland need real time between input events.
        auto settle = [](int ms) {
            QDeadlineTimer deadline(ms);
            while (!deadline.hasExpired()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                QThread::msleep(1);
            }
        };
        settle(200);
        sourceWindow->activate();
        input::InputManager input(seat.get());
        input.setEventSink(manager.get());
        input.setBounds({800, 600});
        // Below a GTK header bar, inside the widget that takes part in the drag.
        const QPointF startPosition =
                sourceWindow->surfaceNode()->absolutePosition() + QPoint(60, 85);
        const QPointF endPosition =
                destinationWindow->surfaceNode()->absolutePosition() + QPoint(60, 85);
        input.onPointerMotionAbsolute({}, startPosition);
        settle(100);
        input.onPointerButton({}, BTN_LEFT, true);
        settle(100);
        // Toolkits start a drag past a threshold, in steps.
        for (int i = 1; i <= 5; ++i) {
            input.onPointerMotionAbsolute({}, startPosition + QPoint(6 * i, 4 * i));
            settle(30);
        }
        ASSERT_TRUE(until([&] {
            return source.saw("STARTED");
        }))
                << source.readAllStandardError().constData();
        const QPointF step = (endPosition - startPosition) / 20;
        for (int i = 1; i <= 20; ++i) {
            input.onPointerMotionAbsolute({}, startPosition + step * i);
            settle(30);
        }
        settle(300);
        input.onPointerButton({}, BTN_LEFT, false);
        EXPECT_TRUE(until([&] {
            return destination.saw("PASS");
        }))
                << destination.output.constData()
                << destination.readAllStandardError().constData()
                << source.readAllStandardError().constData();
        EXPECT_TRUE(until([&] {
            return source.saw("FINISHED");
        }))
                << source.output.constData() << source.readAllStandardError().constData();
        EXPECT_FALSE(seat->drag()->active());
        wl_display_destroy_clients(server->display());
    }
};

// Real toolkits pick the scale up from Xft.dpi and XSETTINGS and draw at
// device resolution: the X11 window is twice their logical size.
TEST_F(XwaylandDndToolkitTest, ToolkitsDrawAtTheXwaylandScale)
{
    xwayland->setScale(2);
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    for (const bool gtk : {false, true}) {
        SCOPED_TRACE(gtk ? "GTK" : "Qt");
        Child client;
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto *name : {"QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                 "QT_QPA_PLATFORMTHEME", "QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS",
                                 "QT_AUTO_SCREEN_SCALE_FACTOR", "GDK_SCALE", "GDK_DPI_SCALE",
                                 "LD_PRELOAD", "WAYLAND_SOCKET"})
            env.remove(QString::fromLatin1(name));
        env.insert("DISPLAY", xwayland->display());
        env.insert("XAUTHORITY", xwayland->authority());
        env.insert("QT_QPA_PLATFORM", "xcb");
        env.insert("GDK_BACKEND", "x11");
        env.insert("WAYLAND_DISPLAY", "nonexistent-native-display");
        env.insert("XDG_RUNTIME_DIR", runtime.path());
        client.setProcessEnvironment(env);
        client.start(gtk ? QStringLiteral("/usr/bin/python3") : QStringLiteral(QT_DND_CLIENT),
                     gtk ? QStringList {QStringLiteral(GTK_DND_CLIENT), QStringLiteral("scaled")}
                         : QStringList {QStringLiteral("scaled")});
        wm::Window *window = nullptr;
        ASSERT_TRUE(until([&] {
            for (auto *w : manager->windows()) {
                if (w->title() == "scaled" && w->rootSurface()->hasContents())
                    window = w;
            }
            return window != nullptr;
        }))
                << client.readAllStandardError().constData();
        const QSize buffer = window->rootSurface()->bufferSize();
        const QSize content = window->rootSurface()->contentSize();
        EXPECT_EQ(content, QSize(qRound(buffer.width() / 2.0), qRound(buffer.height() / 2.0)));
        // Qt sizes the client area exactly; GTK adds its own decoration.
        if (gtk) {
            EXPECT_GE(buffer.width(), 360);
            EXPECT_GE(buffer.height(), 240);
        } else {
            EXPECT_EQ(buffer, QSize(360, 240));
            EXPECT_EQ(content, QSize(180, 120));
        }
        client.kill();
        client.waitForFinished(2000);
        ASSERT_TRUE(until([&] {
            return manager->windows().empty();
        }));
    }
}

// A scale change reaches running toolkits through XSETTINGS: they redraw at the
// new scale into the window the XWM has already resized.
TEST_F(XwaylandDndToolkitTest, RunningToolkitsFollowAScaleChange)
{
    ASSERT_TRUE(start()) << xwayland->error().toStdString();
    // Xwayland waits for frame callbacks before it commits again.
    QTimer frames;
    QObject::connect(&frames, &QTimer::timeout, &frames, [&] {
        compositor->frameSubmitted();
        compositor->frameDrawn(wayland::nowInMilliseconds());
    });
    frames.start(5);
    for (const bool gtk : {false, true}) {
        SCOPED_TRACE(gtk ? "GTK" : "Qt");
        xwayland->setScale(1);
        wayland::updateOutputGlobal(outputGlobal, &output);
        Child client;
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto *name : {"QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                 "QT_QPA_PLATFORMTHEME", "QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS",
                                 "QT_AUTO_SCREEN_SCALE_FACTOR", "GDK_SCALE", "GDK_DPI_SCALE",
                                 "LD_PRELOAD", "WAYLAND_SOCKET"})
            env.remove(QString::fromLatin1(name));
        env.insert("DISPLAY", xwayland->display());
        env.insert("XAUTHORITY", xwayland->authority());
        env.insert("QT_QPA_PLATFORM", "xcb");
        env.insert("GDK_BACKEND", "x11");
        env.insert("WAYLAND_DISPLAY", "nonexistent-native-display");
        env.insert("XDG_RUNTIME_DIR", runtime.path());
        client.setProcessEnvironment(env);
        client.start(gtk ? QStringLiteral("/usr/bin/python3") : QStringLiteral(QT_DND_CLIENT),
                     gtk ? QStringList {QStringLiteral(GTK_DND_CLIENT), QStringLiteral("scaled")}
                         : QStringList {QStringLiteral("scaled")});
        wm::Window *window = nullptr;
        ASSERT_TRUE(until([&] {
            for (auto *w : manager->windows()) {
                if (w->title() == "scaled" && w->rootSurface()->hasContents())
                    window = w;
            }
            return window != nullptr;
        }))
                << client.readAllStandardError().constData();
        const QSize logical = window->rootSurface()->contentSize();
        EXPECT_EQ(window->rootSurface()->bufferSize(), logical);

        xwayland->setScale(2);
        wayland::updateOutputGlobal(outputGlobal, &output);
        // Same logical size, twice the pixels, without restarting the client.
        // Qt may settle a pixel short while its size increments follow the
        // new scale; the renderer keeps that sharp.
        EXPECT_TRUE(until([&] {
            const QSize buffer = window->rootSurface()->bufferSize();
            return std::abs(buffer.width() - logical.width() * 2) <= 1
                    && std::abs(buffer.height() - logical.height() * 2) <= 1
                    && window->rootSurface()->contentSize() == logical;
        }))
                << "buffer " << window->rootSurface()->bufferSize().width() << "x"
                << window->rootSurface()->bufferSize().height() << " content "
                << window->rootSurface()->contentSize().width() << "x"
                << window->rootSurface()->contentSize().height();
        client.kill();
        client.waitForFinished(2000);
        ASSERT_TRUE(until([&] {
            return manager->windows().empty();
        }));
    }
}

TEST_F(XwaylandDndToolkitTest, QtX11ToGtkWayland)
{
    matrix(true, false, true);
}

TEST_F(XwaylandDndToolkitTest, GtkX11ToQtWayland)
{
    matrix(true, true, false);
}

TEST_F(XwaylandDndToolkitTest, QtWaylandToGtkX11)
{
    matrix(false, false, true);
}

TEST_F(XwaylandDndToolkitTest, GtkWaylandToQtX11)
{
    matrix(false, true, false);
}

class XwaylandApplicationTest : public XwaylandTest
{
protected:
    void exercise(const QString &executable, const QStringList &args)
    {
        ASSERT_TRUE(start()) << xwayland->error().toStdString();
        QProcess client;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("DISPLAY", xwayland->display());
        env.insert("XAUTHORITY", xwayland->authority());
        env.insert("GDK_BACKEND", "x11");
        env.insert("QT_QPA_PLATFORM", "xcb");
        env.insert("WAYLAND_DISPLAY", "nonexistent-native-display");
        env.remove("WAYLAND_SOCKET");
        client.setProcessEnvironment(env);
        client.start(executable, args);
        ASSERT_TRUE(until([&] {
            return !manager->windows().empty() || client.state() == QProcess::NotRunning;
        }));
        ASSERT_FALSE(manager->windows().empty()) << client.readAllStandardError().toStdString();
        auto *window = manager->windows()[0];
        EXPECT_TRUE(window->rootSurface()->hasContents());
        EXPECT_FALSE(window->applicationId().isEmpty());
        EXPECT_EQ(window->processId(), client.processId());
        EXPECT_FALSE(window->title().isEmpty());
        {
            const auto devicePath = qgetenv("CUTEFISH_TEST_DRM_DEVICE");
            int drmFd =
                    devicePath.isEmpty() ? -1 : open(devicePath.constData(), O_RDWR | O_CLOEXEC);
            gbm_device *device = drmFd >= 0 ? gbm_create_device(drmFd) : nullptr;
            auto deviceCleanup = qScopeGuard([&] {
                if (device)
                    gbm_device_destroy(device);
                if (drmFd >= 0)
                    close(drmFd);
            });
            ASSERT_TRUE(devicePath.isEmpty() || device);
            auto renderer = device ? render::EglRenderer::create(device, DRM_FORMAT_XRGB8888)
                                   : render::EglRendererTestAccess::create();
            ASSERT_NE(renderer, nullptr);
            std::unique_ptr<gbm_surface, decltype(&gbm_surface_destroy)> gbmSurface(
                    device ? gbm_surface_create(device, 800, 600, DRM_FORMAT_XRGB8888,
                                                GBM_BO_USE_RENDERING)
                           : nullptr,
                    gbm_surface_destroy);
            std::unique_ptr<render::EglRenderTarget> target;
            if (device) {
                ASSERT_TRUE(gbmSurface);
                target = renderer->createTarget(gbmSurface.get(), QSize(800, 600));
            } else {
                const EGLint attrs[] = {EGL_WIDTH, 800, EGL_HEIGHT, 600, EGL_NONE};
                auto surface = eglCreatePbufferSurface(renderer->eglDisplay(),
                                                       renderer->eglConfig(), attrs);
                ASSERT_NE(surface, EGL_NO_SURFACE);
                target = std::make_unique<render::EglRenderTarget>(renderer->eglDisplay(), surface,
                                                                   nullptr, QSize(800, 600));
            }
            ASSERT_TRUE(target);
            auto cleanup = qScopeGuard([&] {
                scene.root()->releaseRenderResources();
                renderer->clearTextureCache();
                renderer->releaseTarget(target.get());
            });
            ASSERT_TRUE(renderer->beginFrame(target.get()));
            static_cast<scene::WindowAnimation *>(window->node())->finish();
            QImage pixels(800, 600, QImage::Format_RGBA8888);
            uint64_t generation = 0;
            ASSERT_TRUE(until([&] {
                compositor->updateTextures(*renderer);
                if (!window->rootSurface()->node()->texture())
                    return false;
                renderer->clear({0.12f, 0.12f, 0.12f, 1});
                scene.root()->render(*renderer);
                glReadPixels(0, 0, 800, 600, GL_RGBA, GL_UNSIGNED_BYTE, pixels.bits());
                compositor->frameSubmitted(++generation);
                compositor->frameDrawn(generation * 16, generation);
                server->flush();
                if (QFileInfo(executable).fileName() != "xterm")
                    return true;
                int ink = 0;
                const auto clientArea = window->contentBounds()
                                                .adjusted(4, 4, -4, -4)
                                                .intersected(QRect(0, 0, 800, 600));
                for (int y = clientArea.top(); y <= clientArea.bottom(); ++y) {
                    for (int x = clientArea.left(); x <= clientArea.right(); ++x) {
                        if (pixels.pixelColor(x, 599 - y).red() < 100)
                            ++ink;
                    }
                }
                return ink > 200;
            }));
            EXPECT_EQ(glGetError(), GLenum(GL_NO_ERROR));
        }
        window->requestBounds(QRect(70, 80, 420, 340));
        window->maximize();
        EXPECT_EQ(window->stateType(), wm::WindowStateType::Maximized);
        window->minimize();
        EXPECT_EQ(manager->windows().size(), 1u);
        window->restore();
        window->setFullscreen(true);
        EXPECT_EQ(window->stateType(), wm::WindowStateType::Fullscreen);
        window->setFullscreen(false);
        window->requestClose();
        EXPECT_TRUE(until([&] {
            return manager->windows().empty();
        }));
        if (client.state() != QProcess::NotRunning) {
            client.terminate();
            EXPECT_TRUE(until([&] {
                return client.state() == QProcess::NotRunning;
            }));
        }
    }
};

TEST_F(XwaylandApplicationTest, GtkX11)
{
    const auto executable = QStandardPaths::findExecutable("gtk3-demo");
    if (executable.isEmpty())
        GTEST_SKIP() << "gtk3-demo unavailable";
    exercise(executable, {});
}

TEST_F(XwaylandApplicationTest, Xterm)
{
    const auto executable =
            qEnvironmentVariable("CUTEFISH_XTERM", QStandardPaths::findExecutable("xterm"));
    if (executable.isEmpty())
        GTEST_SKIP() << "xterm unavailable; set CUTEFISH_XTERM to a test binary";
    exercise(executable,
             {"-title", "Cutefish X11 terminal", "-e", "/bin/sh", "-c",
              "printf 'Cutefish X11 rendering and input\\n'; exec cat"});
}

class XwaylandProtocolTest : public XwaylandTest
{
protected:
    void SetUp() override
    {
        XwaylandTest::SetUp();
        int sockets[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        authorized = wl_client_create(server->display(), sockets[0]);
        shell = std::make_unique<xwayland::XwaylandShell>(server->display(), privileged.get(),
                                                          authorized, &association);
        client = wl_display_connect_to_fd(sockets[1]);
        registry = wl_display_get_registry(client);
        static const wl_registry_listener listener = {
                [](void *data, wl_registry *registry, uint32_t id, const char *name, uint32_t) {
                    auto *self = static_cast<XwaylandProtocolTest *>(data);
                    if (strcmp(name, "wl_compositor") == 0)
                        self->clientCompositor = static_cast<wl_compositor *>(
                                wl_registry_bind(registry, id, &wl_compositor_interface, 4));
                    if (strcmp(name, "xwayland_shell_v1") == 0) {
                        self->globalId = id;
                        self->clientShell = static_cast<xwayland_shell_v1 *>(
                                wl_registry_bind(registry, id, &xwayland_shell_v1_interface, 1));
                    }
                },
                [](void *, wl_registry *, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, this);
        ASSERT_GE(sync(), 0);
        ASSERT_NE(clientShell, nullptr);
        ASSERT_GE(sync(), 0);
    }

    int sync()
    {
        auto future = std::async(std::launch::async, [&] {
            return wl_display_roundtrip(client);
        });
        if (!until([&] {
                return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            })) {
            shutdown(wl_display_get_fd(client), SHUT_RDWR);
            ADD_FAILURE() << "protocol roundtrip timed out";
        }
        return future.get();
    }

    void TearDown() override
    {
        if (clientShell)
            xwayland_shell_v1_destroy(clientShell);
        if (clientCompositor)
            wl_compositor_destroy(clientCompositor);
        if (registry)
            wl_registry_destroy(registry);
        wl_display_disconnect(client);
        wl_display_destroy_clients(server->display());
        shell.reset();
        XwaylandTest::TearDown();
    }

    xwayland::SurfaceAssociation association;
    std::unique_ptr<xwayland::XwaylandShell> shell;
    wl_client *authorized = nullptr;
    wl_display *client = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *clientCompositor = nullptr;
    xwayland_shell_v1 *clientShell = nullptr;
    uint32_t globalId = 0;
};

TEST_F(XwaylandProtocolTest, CommitTimingAndWrapperLifetime)
{
    auto *surface = wl_compositor_create_surface(clientCompositor);
    ASSERT_GE(sync(), 0);
    ASSERT_EQ(compositor->surfaces().size(), 1u);
    EXPECT_TRUE(compositor->surfaces()[0]->isHidden());
    auto *role = xwayland_shell_v1_get_xwayland_surface(clientShell, surface);
    xwayland_surface_v1_set_serial(role, 7, 9);
    ASSERT_GE(sync(), 0);
    EXPECT_EQ(association.pendingCount(), 0u);
    ASSERT_EQ(compositor->surfaces().size(), 1u);
    EXPECT_TRUE(compositor->surfaces()[0]->isHidden());
    wl_surface_commit(surface);
    ASSERT_GE(sync(), 0);
    EXPECT_EQ(association.pendingCount(), 1u);
    xwayland_surface_v1_destroy(role);
    ASSERT_GE(sync(), 0);
    EXPECT_EQ(association.pendingCount(), 1u);
    EXPECT_TRUE(compositor->surfaces()[0]->hasRole());
    uint32_t matched = 0;
    association.associated = [&](auto id, auto *) {
        matched = id;
    };
    EXPECT_TRUE(association.addWindow(association.serial(7, 9), 77));
    EXPECT_EQ(matched, 77u);
    wl_surface_destroy(surface);
    ASSERT_GE(sync(), 0);
    EXPECT_TRUE(compositor->surfaces().empty());
}

TEST_F(XwaylandProtocolTest, ZeroSerialRejected)
{
    auto *surface = wl_compositor_create_surface(clientCompositor);
    auto *role = xwayland_shell_v1_get_xwayland_surface(clientShell, surface);
    xwayland_surface_v1_set_serial(role, 0, 0);
    EXPECT_EQ(sync(), -1);
    const wl_interface *interface = nullptr;
    EXPECT_EQ(wl_display_get_protocol_error(client, &interface, nullptr),
              XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL);
    EXPECT_STREQ(interface->name, "xwayland_surface_v1");
    wl_proxy_destroy(reinterpret_cast<wl_proxy *>(role));
    wl_proxy_destroy(reinterpret_cast<wl_proxy *>(surface));
}

TEST_F(XwaylandProtocolTest, RepeatedCommitRejected)
{
    auto *surface = wl_compositor_create_surface(clientCompositor);
    auto *role = xwayland_shell_v1_get_xwayland_surface(clientShell, surface);
    xwayland_surface_v1_set_serial(role, 1, 0);
    wl_surface_commit(surface);
    ASSERT_GE(sync(), 0);
    xwayland_surface_v1_set_serial(role, 2, 0);
    ASSERT_GE(sync(), 0);
    wl_surface_commit(surface);
    EXPECT_EQ(sync(), -1);
    EXPECT_EQ(wl_display_get_protocol_error(client, nullptr, nullptr),
              XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED);
    wl_proxy_destroy(reinterpret_cast<wl_proxy *>(role));
    wl_proxy_destroy(reinterpret_cast<wl_proxy *>(surface));
}

// set_serial carries Xwayland's serials in order, but the surfaces may commit
// them in any order; a serial that does not increase is refused.
TEST_F(XwaylandProtocolTest, SerialsIncreaseButCommitInAnyOrder)
{
    std::vector<wl_proxy *> proxies;
    const auto withSerial = [&](uint32_t serial) {
        auto *surface = wl_compositor_create_surface(clientCompositor);
        auto *role = xwayland_shell_v1_get_xwayland_surface(clientShell, surface);
        xwayland_surface_v1_set_serial(role, serial, 0);
        proxies.push_back(reinterpret_cast<wl_proxy *>(role));
        proxies.push_back(reinterpret_cast<wl_proxy *>(surface));
        return surface;
    };
    auto *first = withSerial(1);
    auto *second = withSerial(2);
    wl_surface_commit(second);
    wl_surface_commit(first);
    ASSERT_GE(sync(), 0);
    EXPECT_EQ(association.pendingCount(), 2u);
    EXPECT_TRUE(association.addWindow(1, 11));
    EXPECT_TRUE(association.addWindow(2, 12));
    EXPECT_EQ(association.pendingCount(), 0u);
    withSerial(2);
    EXPECT_EQ(sync(), -1);
    const wl_interface *interface = nullptr;
    EXPECT_EQ(wl_display_get_protocol_error(client, &interface, nullptr),
              XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL);
    EXPECT_STREQ(interface->name, "xwayland_surface_v1");
    for (auto *proxy : proxies)
        wl_proxy_destroy(proxy);
}

TEST_F(XwaylandProtocolTest, RegistryFilterAndForgedBind)
{
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    ASSERT_NE(wl_client_create(server->display(), sockets[0]), nullptr);
    auto *stranger = wl_display_connect_to_fd(sockets[1]);
    auto *registry = wl_display_get_registry(stranger);
    bool exposed = false;
    static const wl_registry_listener listener = {
            [](void *data, wl_registry *, uint32_t, const char *name, uint32_t) {
                if (strcmp(name, "xwayland_shell_v1") == 0)
                    *static_cast<bool *>(data) = true;
            },
            [](void *, wl_registry *, uint32_t) {},
    };
    wl_registry_add_listener(registry, &listener, &exposed);
    auto *saved = client;
    client = stranger;
    ASSERT_GE(sync(), 0);
    EXPECT_FALSE(exposed);
    auto *forged = wl_registry_bind(registry, globalId, &xwayland_shell_v1_interface, 1);
    EXPECT_EQ(sync(), -1);
    wl_proxy_destroy(static_cast<wl_proxy *>(forged));
    wl_registry_destroy(registry);
    wl_display_disconnect(stranger);
    client = saved;
}

} // namespace
