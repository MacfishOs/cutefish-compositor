// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// End-to-end tests for the two private Cutefish shell protocols, driven through
// real libwayland-client connections: one trusted shell connection created the
// way the session creates it, and one ordinary application connection.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <linux/input-event-codes.h>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <appmenu-client-protocol.h>
#include <cutefish-window-hints-v1-client-protocol.h>
#include "wayland/protocols/window_hints.h"
#include "wm/mru_window_tracker.h"
#include <cutefish-shell-v1-client-protocol.h>
#include <cutefish-window-management-v1-client-protocol.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <xdg-shell-client-protocol.h>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "shm_test_util.h"
#include "wayland/privileged_client.h"
#include "wayland/protocols/application_menu.h"
#include "wayland/protocols/cutefish_shell.h"
#include "wayland/protocols/cutefish_window_management.h"
#include "wayland/protocols/wl_compositor.h"
#include "wayland/protocols/wl_seat.h"
#include "wayland/protocols/wl_shm.h"
#include "wayland/protocols/wl_subcompositor.h"
#include "wayland/protocols/xdg_shell.h"
#include "wayland/server.h"
#include "wm/activation.h"
#include "wm/stacking.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

using namespace cutefish;

namespace {

struct RunningClient {
    std::atomic<int> &count;

    ~RunningClient() { --count; }
};

constexpr QRect kOutput(0, 0, 800, 600);

// What the client learned about one managed window.
struct TrackedWindow {
    cutefish_window_v1 *object = nullptr;
    quint64 id = 0;
    quint64 parentId = 0;
    std::string title;
    std::string appId;
    std::string menuService;
    std::string menuPath;
    uint32_t pid = 0;
    bool active = false;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
    bool hidden = false;
    bool skipTaskbar = false;
    int skipTaskbarCount = 0;
    QRect geometry;
    QImage thumbnail;
    int doneCount = 0;
    int parentCount = 0;
    int minimizeLandedCount = 0;
    uint32_t desk = 0;
    bool closed = false;
};

struct Client;

struct XdgWindow {
    wl_surface *surface = nullptr;
    xdg_surface *xdgSurface = nullptr;
    xdg_toplevel *toplevel = nullptr;
    cutefish_shell_surface_v1 *shellSurface = nullptr;
    wl_buffer *buffer = nullptr;
    std::vector<uint32_t> surfaceConfigures;
    std::vector<QSize> bounds;
    int closes = 0;

    uint32_t lastSerial() const { return surfaceConfigures.back(); }
};

const xdg_surface_listener surfaceListener = {
        [](void *data, xdg_surface *, uint32_t serial) {
            static_cast<XdgWindow *>(data)->surfaceConfigures.push_back(serial);
        },
};

const xdg_toplevel_listener toplevelListener = {
        [](void *, xdg_toplevel *, int32_t, int32_t, wl_array *) {},
        [](void *data, xdg_toplevel *) {
            ++static_cast<XdgWindow *>(data)->closes;
        },
        [](void *data, xdg_toplevel *, int32_t width, int32_t height) {
            static_cast<XdgWindow *>(data)->bounds.emplace_back(width, height);
        },
        [](void *, xdg_toplevel *, wl_array *) {},
};

const cutefish_window_v1_listener windowListener = {
        [](void *data, cutefish_window_v1 *, const char *title) {
            static_cast<TrackedWindow *>(data)->title = title;
        },
        [](void *data, cutefish_window_v1 *, const char *appId) {
            static_cast<TrackedWindow *>(data)->appId = appId;
        },
        [](void *data, cutefish_window_v1 *, uint32_t pid) {
            static_cast<TrackedWindow *>(data)->pid = pid;
        },
        [](void *data, cutefish_window_v1 *, uint32_t active, uint32_t minimized,
           uint32_t maximized, uint32_t fullscreen, uint32_t hidden) {
            auto *window = static_cast<TrackedWindow *>(data);
            window->active = active != 0;
            window->minimized = minimized != 0;
            window->maximized = maximized != 0;
            window->fullscreen = fullscreen != 0;
            window->hidden = hidden != 0;
        },
        [](void *data, cutefish_window_v1 *, int32_t x, int32_t y, int32_t width, int32_t height) {
            static_cast<TrackedWindow *>(data)->geometry = QRect(x, y, width, height);
        },
        [](void *data, cutefish_window_v1 *) {
            ++static_cast<TrackedWindow *>(data)->doneCount;
        },
        [](void *data, cutefish_window_v1 *) {
            static_cast<TrackedWindow *>(data)->closed = true;
        },
        [](void *data, cutefish_window_v1 *, const char *service, const char *path) {
            auto *window = static_cast<TrackedWindow *>(data);
            window->menuService = service;
            window->menuPath = path;
        },
        [](void *data, cutefish_window_v1 *, int32_t fd, uint32_t width, uint32_t height) {
            const size_t size = size_t(width) * height * 4;
            void *pixels = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            ASSERT_NE(pixels, MAP_FAILED);
            static_cast<TrackedWindow *>(data)->thumbnail =
                    QImage(static_cast<const uchar *>(pixels), int(width), int(height),
                           QImage::Format_ARGB32_Premultiplied)
                            .copy();
            munmap(pixels, size);
        },
        [](void *data, cutefish_window_v1 *) {
            ++static_cast<TrackedWindow *>(data)->minimizeLandedCount;
        },
        [](void *data, cutefish_window_v1 *, uint32_t high, uint32_t low) {
            auto *window = static_cast<TrackedWindow *>(data);
            window->parentId = (quint64(high) << 32) | low;
            ++window->parentCount;
        },
        [](void *data, cutefish_window_v1 *, uint32_t skip) {
            auto *window = static_cast<TrackedWindow *>(data);
            window->skipTaskbar = skip != 0;
            ++window->skipTaskbarCount;
        },
        [](void *data, cutefish_window_v1 *, uint32_t desk) {
            static_cast<TrackedWindow *>(data)->desk = desk;
        },
};

struct Client {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *wmBase = nullptr;
    cutefish_shell_v1 *shell = nullptr;
    cutefish_window_management_v1 *management = nullptr;
    std::vector<cutefish_window_management_v1 *> retiredManagements;
    std::map<std::string, std::pair<uint32_t, uint32_t>> globals;

    std::vector<std::unique_ptr<TrackedWindow>> tracked;
    quint64 activeWindowId = 0;
    int initialStateDone = 0;
    struct Pick {
        uint32_t kind = CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_NONE;
        quint64 id = 0;
    };
    std::map<uint32_t, Pick> picks;
    std::vector<uint32_t> desks;
    std::map<uint32_t, std::string> deskNames;
    uint32_t activeDesk = 0;

    std::vector<std::unique_ptr<XdgWindow>> windows;

    explicit Client(int fd)
        : display(wl_display_connect_to_fd(fd))
    {
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener = {
                [](void *data, wl_registry *, uint32_t name, const char *interface,
                   uint32_t version) {
                    static_cast<Client *>(data)->globals[interface] = {name, version};
                },
                [](void *, wl_registry *, uint32_t) {}};
        wl_registry_add_listener(registry, &listener, this);
        roundtrip();

        compositor = static_cast<wl_compositor *>(wl_registry_bind(
                registry, globals["wl_compositor"].first, &wl_compositor_interface, 4));
        shm = static_cast<wl_shm *>(
                wl_registry_bind(registry, globals["wl_shm"].first, &wl_shm_interface, 1));
        wmBase = static_cast<xdg_wm_base *>(wl_registry_bind(registry, globals["xdg_wm_base"].first,
                                                             &xdg_wm_base_interface,
                                                             globals["xdg_wm_base"].second));
        static const xdg_wm_base_listener wmBaseListener = {
                [](void *, xdg_wm_base *base, uint32_t serial) {
                    xdg_wm_base_pong(base, serial);
                }};
        xdg_wm_base_add_listener(wmBase, &wmBaseListener, this);
        roundtrip();
    }

    ~Client()
    {
        for (auto &window : windows)
            destroyWindowObjects(window.get());
        windows.clear();
        for (auto &window : tracked) {
            if (window->object)
                cutefish_window_v1_destroy(window->object);
        }
        tracked.clear();
        if (management)
            cutefish_window_management_v1_destroy(management);
        for (cutefish_window_management_v1 *retired : retiredManagements)
            cutefish_window_management_v1_destroy(retired);
        if (shell)
            cutefish_shell_v1_destroy(shell);
        xdg_wm_base_destroy(wmBase);
        wl_shm_destroy(shm);
        wl_compositor_destroy(compositor);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
    }

    bool bindShell()
    {
        if (!globals.contains("cutefish_shell_v1"))
            return false;
        shell = static_cast<cutefish_shell_v1 *>(wl_registry_bind(
                registry, globals["cutefish_shell_v1"].first, &cutefish_shell_v1_interface,
                globals["cutefish_shell_v1"].second));
        sync();
        return true;
    }

    bool bindManagement()
    {
        if (!globals.contains("cutefish_window_management_v1"))
            return false;
        // A rebinding stands for a restarted shell; the first binding stays
        // usable for the windows it announced and goes with the client.
        if (management)
            retiredManagements.push_back(management);
        management = static_cast<cutefish_window_management_v1 *>(
                wl_registry_bind(registry, globals["cutefish_window_management_v1"].first,
                                 &cutefish_window_management_v1_interface, 1));
        static const cutefish_window_management_v1_listener listener = {
                [](void *data, cutefish_window_management_v1 *, cutefish_window_v1 *object,
                   uint32_t idHi, uint32_t idLo) {
                    auto *client = static_cast<Client *>(data);
                    client->tracked.push_back(std::make_unique<TrackedWindow>());
                    TrackedWindow *window = client->tracked.back().get();
                    window->object = object;
                    window->id = (static_cast<quint64>(idHi) << 32) | idLo;
                    cutefish_window_v1_add_listener(object, &windowListener, window);
                },
                [](void *data, cutefish_window_management_v1 *, uint32_t idHi, uint32_t idLo) {
                    static_cast<Client *>(data)->activeWindowId =
                            (static_cast<quint64>(idHi) << 32) | idLo;
                },
                [](void *data, cutefish_window_management_v1 *) {
                    ++static_cast<Client *>(data)->initialStateDone;
                },
                [](void *data, cutefish_window_management_v1 *, uint32_t serial, uint32_t kind,
                   uint32_t idHi, uint32_t idLo) {
                    static_cast<Client *>(data)->picks[serial] = {
                            kind, (static_cast<quint64>(idHi) << 32) | idLo};
                },
                [](void *data, cutefish_window_management_v1 *, uint32_t desk) {
                    static_cast<Client *>(data)->desks.push_back(desk);
                },
                [](void *data, cutefish_window_management_v1 *, uint32_t desk) {
                    auto *client = static_cast<Client *>(data);
                    std::erase(client->desks, desk);
                    client->deskNames.erase(desk);
                },
                [](void *data, cutefish_window_management_v1 *, uint32_t desk, const char *name) {
                    static_cast<Client *>(data)->deskNames[desk] = name;
                },
                [](void *data, cutefish_window_management_v1 *, uint32_t desk) {
                    static_cast<Client *>(data)->activeDesk = desk;
                },
        };
        cutefish_window_management_v1_add_listener(management, &listener, this);
        sync();
        return true;
    }

    TrackedWindow *trackedById(quint64 id)
    {
        for (auto &window : tracked) {
            if (window->id == id && !window->closed)
                return window.get();
        }
        return nullptr;
    }

    // Live entries only, in the order they were announced.
    std::vector<TrackedWindow *> liveTracked()
    {
        std::vector<TrackedWindow *> live;
        for (auto &window : tracked) {
            if (!window->closed)
                live.push_back(window.get());
        }
        return live;
    }

    wl_buffer *makeBuffer(const QSize &size)
    {
        const int stride = size.width() * 4;
        const size_t bytes = static_cast<size_t>(stride) * size.height();
        const int fd = testsupport::makeMemfd(bytes);
        wl_shm_pool *pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(bytes));
        wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, size.width(), size.height(), stride,
                                                      WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return buffer;
    }

    XdgWindow *makeToplevel(const char *title, const char *appId)
    {
        windows.push_back(std::make_unique<XdgWindow>());
        XdgWindow *window = windows.back().get();
        window->surface = wl_compositor_create_surface(compositor);
        window->xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, window->surface);
        xdg_surface_add_listener(window->xdgSurface, &surfaceListener, window);
        window->toplevel = xdg_surface_get_toplevel(window->xdgSurface);
        xdg_toplevel_add_listener(window->toplevel, &toplevelListener, window);
        if (title)
            xdg_toplevel_set_title(window->toplevel, title);
        if (appId)
            xdg_toplevel_set_app_id(window->toplevel, appId);
        return window;
    }

    void map(XdgWindow *window, const QSize &size)
    {
        wl_surface_commit(window->surface);
        sync();
        xdg_surface_ack_configure(window->xdgSurface, window->lastSerial());
        if (!window->buffer)
            window->buffer = makeBuffer(size);
        wl_surface_attach(window->surface, window->buffer, 0, 0);
        wl_surface_damage(window->surface, 0, 0, size.width(), size.height());
        wl_surface_commit(window->surface);
        sync();
    }

    void destroyWindowObjects(XdgWindow *window)
    {
        if (window->shellSurface) {
            cutefish_shell_surface_v1_destroy(window->shellSurface);
            window->shellSurface = nullptr;
        }
        if (window->toplevel) {
            xdg_toplevel_destroy(window->toplevel);
            window->toplevel = nullptr;
        }
        if (window->xdgSurface) {
            xdg_surface_destroy(window->xdgSurface);
            window->xdgSurface = nullptr;
        }
        if (window->surface) {
            wl_surface_destroy(window->surface);
            window->surface = nullptr;
        }
        if (window->buffer) {
            wl_buffer_destroy(window->buffer);
            window->buffer = nullptr;
        }
    }

    bool waitFor(const std::function<bool()> &predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline || roundtrip() < 0)
                return false;
        }
        return true;
    }

    int roundtrip()
    {
        bool done = false;
        auto *callback = wl_display_sync(display);
        if (!callback)
            return -1;
        static const wl_callback_listener listener = {[](void *data, wl_callback *, uint32_t) {
            *static_cast<bool *>(data) = true;
        }};
        wl_callback_add_listener(callback, &listener, &done);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done && !wl_display_get_error(display)) {
            if (wl_display_dispatch_pending(display) < 0 || done)
                break;
            if (wl_display_prepare_read(display) != 0)
                continue;
            wl_display_flush(display);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           deadline - std::chrono::steady_clock::now())
                                           .count();
            pollfd fd {wl_display_get_fd(display), POLLIN, 0};
            if (remaining <= 0 || poll(&fd, 1, static_cast<int>(remaining)) <= 0) {
                wl_display_cancel_read(display);
                break;
            }
            if (wl_display_read_events(display) < 0)
                break;
        }
        wl_callback_destroy(callback);
        return done ? 0 : -1;
    }

    void sync() { roundtrip(); }

    uint32_t syncExpectingError()
    {
        if (roundtrip() >= 0)
            return 0;
        if (wl_display_get_error(display) != EPROTO)
            return 0;
        const wl_interface *raisedOn = nullptr;
        uint32_t id = 0;
        return wl_display_get_protocol_error(display, &raisedOn, &id);
    }
};

class PrivateShellProtocolTest : public testing::Test
{
protected:
    scene::Scene scene;
    scene::Node *surfaceContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    scene::Node *cursorContainer =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("cursor")));
    compositor::Compositor compositor {surfaceContainer};
    input::Seat seat {&compositor};
    input::InputManager manager {&seat};
    wm::WindowManager windowManager {&compositor, &seat};
    wayland::Server server;

    std::mutex mutex;
    std::deque<std::function<void()>> pendingChecks;

    void SetUp() override
    {
        scene.setSize(kOutput.size());
        seat.setCursorContainer(cursorContainer);
        manager.setBounds(kOutput.size());
        manager.setEventSink(&windowManager);
        windowManager.setOutputBounds(kOutput);
    }

    // Runs `body` on the server thread and waits for it.
    void check(std::function<void()> body)
    {
        struct Work {
            std::mutex mutex;
            std::promise<void> done;
            bool cancelled = false;
        };

        auto state = std::make_shared<Work>();
        auto future = state->done.get_future();
        {
            std::lock_guard lock(mutex);
            pendingChecks.emplace_back([state, body = std::move(body)] {
                std::lock_guard lock(state->mutex);
                if (!state->cancelled)
                    body();
                state->done.set_value();
            });
        }
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            std::lock_guard lock(state->mutex);
            state->cancelled = true;
            ADD_FAILURE() << "server check did not execute before its deadline";
        }
    }

    bool awaitRole(wm::WindowRole role)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool mapped = false;
        while (!mapped && std::chrono::steady_clock::now() < deadline)
            check([&] {
                mapped = windowWithRole(role) != nullptr;
            });
        return mapped;
    }

    wm::Window *windowWithRole(wm::WindowRole role)
    {
        for (wm::Window *window : windowManager.windows()) {
            if (window->role() == role)
                return window;
        }
        return nullptr;
    }

    // `shellBody` runs on a trusted connection created the way the session
    // creates one; `appBody` runs on an ordinary connection to the public
    // socket. Either may be empty.
    void run(std::function<void(Client &)> shellBody, std::function<void(Client &)> appBody = {})
    {
        auto privileged = std::make_unique<wayland::PrivilegedClients>(server.display());
        auto waylandSeat = std::make_unique<wayland::WaylandSeat>(&seat, server.serialTracker());
        auto xdgShell = std::make_unique<wayland::WaylandXdgShell>(&compositor, &windowManager,
                                                                   &seat, server.serialTracker());
        auto cutefishShell =
                std::make_unique<wayland::WaylandCutefishShell>(&windowManager, privileged.get());
        auto management = std::make_unique<wayland::WaylandCutefishWindowManagement>(
                &windowManager, privileged.get());

        ASSERT_TRUE(wayland::createCompositorGlobal(server.display(), &compositor));
        ASSERT_TRUE(wayland::createSubcompositorGlobal(server.display()));
        ASSERT_TRUE(wayland::createAppMenuGlobal(server.display()));
        ASSERT_TRUE(wayland::createWindowHintsGlobal(server.display()));
        ASSERT_TRUE(wayland::createShmGlobal(server.display(), &compositor));
        ASSERT_TRUE(wayland::createSeatGlobal(server.display(), waylandSeat.get()));
        ASSERT_TRUE(wayland::createXdgShellGlobal(server.display(), xdgShell.get()));
        ASSERT_TRUE(wayland::createCutefishShellGlobal(server.display(), cutefishShell.get()));
        ASSERT_TRUE(
                wayland::createCutefishWindowManagementGlobal(server.display(), management.get()));
        privileged->setShellDisconnectedCallback([this] {
            windowManager.clearAllExclusiveZones();
        });

        int shellFds[2];
        ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, shellFds));
        ASSERT_TRUE(privileged->adoptShellConnection(shellFds[0]));

        int appFds[2] = {-1, -1};
        if (appBody) {
            ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, appFds));
            ASSERT_TRUE(wl_client_create(server.display(), appFds[0]));
        }

        std::atomic<int> running = appBody ? 2 : 1;
        std::mutex lifetimeMutex;
        std::condition_variable lifetimeChanged;
        int unfinishedBodies = running;
        auto retainUntilAssertionsFinish = [&] {
            std::unique_lock lock(lifetimeMutex);
            --unfinishedBodies;
            lifetimeChanged.notify_all();
            EXPECT_TRUE(lifetimeChanged.wait_for(lock, std::chrono::seconds(8), [&] {
                return unfinishedBodies == 0;
            })) << "peer client body did not finish";
        };
        std::thread shellThread([&] {
            {
                Client c(shellFds[1]);
                if (shellBody)
                    shellBody(c);
                retainUntilAssertionsFinish();
            }
            --running;
        });
        std::thread appThread;
        if (appBody) {
            appThread = std::thread([&] {
                {
                    Client c(appFds[1]);
                    appBody(c);
                    retainUntilAssertionsFinish();
                }
                --running;
            });
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (running > 0) {
            server.dispatch(std::chrono::milliseconds(1));
            server.flush();
            std::function<void()> work;
            {
                std::lock_guard lock(mutex);
                if (!pendingChecks.empty()) {
                    work = std::move(pendingChecks.front());
                    pendingChecks.pop_front();
                }
            }
            if (work)
                work();
            if (std::chrono::steady_clock::now() > deadline) {
                ADD_FAILURE() << "client timed out";
                break;
            }
        }
        shellThread.join();
        if (appThread.joinable())
            appThread.join();
        wl_display_destroy_clients(server.display());
        EXPECT_TRUE(windowManager.windows().empty());
        EXPECT_TRUE(compositor.surfaces().empty());
        EXPECT_EQ(privileged->shellClient(), nullptr);
    }
};

////////////////////////////////////////////////////////////////////////////////
// Privilege

TEST_F(PrivateShellProtocolTest, TheTrustedClientSeesBothPrivateGlobals)
{
    run([&](Client &c) {
        EXPECT_TRUE(c.globals.contains("cutefish_shell_v1"));
        EXPECT_TRUE(c.globals.contains("cutefish_window_management_v1"));
        // Every public global is still there for it.
        EXPECT_TRUE(c.globals.contains("wl_compositor"));
        EXPECT_TRUE(c.globals.contains("xdg_wm_base"));
        EXPECT_TRUE(c.globals.contains("wl_seat"));
    });
}

TEST_F(PrivateShellProtocolTest, AnOrdinaryClientNeverSeesThePrivateGlobals)
{
    run({}, [&](Client &c) {
        EXPECT_FALSE(c.globals.contains("cutefish_shell_v1"));
        EXPECT_FALSE(c.globals.contains("cutefish_window_management_v1"));
        EXPECT_TRUE(c.globals.contains("wl_compositor"));
        EXPECT_TRUE(c.globals.contains("xdg_wm_base"));
    });
}

TEST_F(PrivateShellProtocolTest, AnOrdinaryClientCannotBindThePrivateGlobalsByName)
{
    // The name is guessed the way a hostile client would have to guess it: the
    // shell learns the real one, and the application tries to bind it anyway.
    std::promise<std::pair<uint32_t, uint32_t>> namePromise;
    auto nameFuture = namePromise.get_future();

    run(
            [&](Client &c) {
                namePromise.set_value(c.globals["cutefish_shell_v1"]);
            },
            [&](Client &c) {
                ASSERT_EQ(nameFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
                const auto [name, version] = nameFuture.get();
                auto *shell = static_cast<cutefish_shell_v1 *>(
                        wl_registry_bind(c.registry, name, &cutefish_shell_v1_interface, version));
                // libwayland raises wl_display.error rather than a per-interface
                // code, so any protocol error at all is the pass condition.
                EXPECT_NE(0, c.roundtrip());
                EXPECT_EQ(EPROTO, wl_display_get_error(c.display));
                // wl_proxy_destroy rather than the generated destructor: no request
                // can be marshalled on a display that has already errored out.
                wl_proxy_destroy(reinterpret_cast<wl_proxy *>(shell));
            });
}

////////////////////////////////////////////////////////////////////////////////
// Roles, position and stacking

TEST_F(PrivateShellProtocolTest, TheRoleIsSetBeforeTheWindowIsFirstMapped)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindShell());
        XdgWindow *window = c.makeToplevel("Dock", "cutefish-shell");
        window->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, window->surface);
        cutefish_shell_surface_v1_set_role(window->shellSurface,
                                           CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK);
        cutefish_shell_surface_v1_set_position(window->shellSurface, 100, 500);
        c.map(window, QSize(600, 60));

        check([&] {
            wm::Window *dock = windowWithRole(wm::WindowRole::Dock);
            ASSERT_NE(nullptr, dock);
            // Placed exactly where it asked, not cascaded, and undecorated.
            EXPECT_EQ(QPoint(100, 500), dock->bounds().topLeft());
            EXPECT_FALSE(dock->isDecorated());
        });
    });
}

TEST_F(PrivateShellProtocolTest, AShellSurfaceGetsNoConfigureBounds)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindShell());
        // Qt clamps a size it picks itself to the bounds, which would shrink
        // the desktop off the strip the dock reserves.
        XdgWindow *desktop = c.makeToplevel("Desktop", "cutefish-shell");
        desktop->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, desktop->surface);
        cutefish_shell_surface_v1_set_role(desktop->shellSurface,
                                           CUTEFISH_SHELL_SURFACE_V1_ROLE_DESKTOP);
        c.map(desktop, QSize(800, 600));
        XdgWindow *app = c.makeToplevel("App", "app");
        c.map(app, QSize(300, 200));

        ASSERT_FALSE(desktop->bounds.empty());
        for (const QSize &bounds : desktop->bounds)
            EXPECT_EQ(QSize(0, 0), bounds);
        ASSERT_FALSE(app->bounds.empty());
        EXPECT_FALSE(app->bounds.front().isEmpty());
    });
}

TEST_F(PrivateShellProtocolTest, RoleStackingOrdersTheShellAroundApplications)
{
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.bindShell());
                const std::pair<const char *, uint32_t> roles[] = {
                        {"Desktop", CUTEFISH_SHELL_SURFACE_V1_ROLE_DESKTOP},
                        {"Launcher", CUTEFISH_SHELL_SURFACE_V1_ROLE_LAUNCHER},
                        {"Dock", CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK},
                        {"Notification", CUTEFISH_SHELL_SURFACE_V1_ROLE_NOTIFICATION},
                        {"Capture", CUTEFISH_SHELL_SURFACE_V1_ROLE_CAPTURE},
                        {"Osd", CUTEFISH_SHELL_SURFACE_V1_ROLE_OSD},
                };
                for (const auto &[title, role] : roles) {
                    XdgWindow *window = c.makeToplevel(title, "cutefish-shell");
                    window->shellSurface =
                            cutefish_shell_v1_get_shell_surface(c.shell, window->surface);
                    cutefish_shell_surface_v1_set_role(window->shellSurface, role);
                    cutefish_shell_surface_v1_set_position(window->shellSurface, 0, 0);
                    c.map(window, QSize(200, 100));
                }

                ASSERT_TRUE(awaitRole(wm::WindowRole::Normal));
                check([&] {
                    const std::vector<wm::Window *> &order = windowManager.windows();
                    ASSERT_EQ(7u, order.size());
                    std::vector<wm::WindowRole> roleOrder;
                    for (wm::Window *window : order)
                        roleOrder.push_back(window->role());
                    EXPECT_EQ(std::vector<wm::WindowRole>({
                                      wm::WindowRole::Desktop,
                                      wm::WindowRole::Normal,
                                      wm::WindowRole::Launcher,
                                      wm::WindowRole::Dock,
                                      wm::WindowRole::Notification,
                                      wm::WindowRole::Osd,
                                      wm::WindowRole::Capture,
                              }),
                              roleOrder);
                });
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
            });
}

TEST_F(PrivateShellProtocolTest, ActivationNeverLiftsAWindowOutOfItsLayer)
{
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.bindShell());
                XdgWindow *dock = c.makeToplevel("Dock", "cutefish-shell");
                dock->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, dock->surface);
                cutefish_shell_surface_v1_set_role(dock->shellSurface,
                                                   CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK);
                cutefish_shell_surface_v1_set_position(dock->shellSurface, 0, 540);
                c.map(dock, QSize(800, 60));
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                ASSERT_TRUE(awaitRole(wm::WindowRole::Dock));
                check([&] {
                    wm::Window *app = nullptr;
                    for (wm::Window *candidate : windowManager.windows()) {
                        if (candidate->role() == wm::WindowRole::Normal)
                            app = candidate;
                    }
                    ASSERT_NE(nullptr, app);
                    // Mapping activated it; raising it again must not put it over
                    // the dock.
                    windowManager.raise(app);
                    EXPECT_EQ(wm::WindowRole::Dock, windowManager.windows().back()->role());
                });
            });
}

TEST_F(PrivateShellProtocolTest, FinishedPeerBodyRetainsItsMappedWindowThroughAssertions)
{
    std::atomic<bool> appBodyFinished = false;
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.waitFor([&] {
                    return appBodyFinished.load();
                }));
                ASSERT_TRUE(awaitRole(wm::WindowRole::Normal));
                check([&] {
                    ASSERT_EQ(windowManager.windows().size(), 1u);
                    EXPECT_EQ(windowManager.windows().front()->role(), wm::WindowRole::Normal);
                });
            },
            [&](Client &c) {
                auto *window = c.makeToplevel("Retained", "app");
                c.map(window, QSize(300, 200));
                appBodyFinished = true;
            });
}

// Qt destroys and recreates the cutefish_shell_surface_v1 every time it
// recreates a window's wl_surface. Losing the role in between would drop the
// dock into the normal layer, where the launcher covers it.
TEST_F(PrivateShellProtocolTest, DestroyingTheShellSurfaceLeavesAMappedWindowInItsLayer)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindShell());
        XdgWindow *dock = c.makeToplevel("Dock", "cutefish-shell");
        dock->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, dock->surface);
        cutefish_shell_surface_v1_set_role(dock->shellSurface, CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK);
        cutefish_shell_surface_v1_set_position(dock->shellSurface, 0, 540);
        cutefish_shell_surface_v1_set_exclusive_zone(dock->shellSurface,
                                                     CUTEFISH_SHELL_SURFACE_V1_EDGE_BOTTOM, 60);
        c.map(dock, QSize(800, 60));
        check([&] {
            ASSERT_NE(nullptr, windowWithRole(wm::WindowRole::Dock));
            EXPECT_EQ(QRect(0, 0, 800, 540), windowManager.workAreaFor(nullptr));
        });

        // Only the extension object goes; the wl_surface and its xdg_toplevel
        // stay mapped.
        cutefish_shell_surface_v1_destroy(dock->shellSurface);
        dock->shellSurface = nullptr;
        c.sync();

        check([&] {
            wm::Window *window = windowWithRole(wm::WindowRole::Dock);
            ASSERT_NE(nullptr, window) << "the mapped dock lost its role";
            EXPECT_EQ(wm::StackingLayer::Dock, wm::layerFor(window));
            // The reservation does go with the object that made it.
            EXPECT_EQ(kOutput, windowManager.workAreaFor(nullptr));
        });
    });
}

////////////////////////////////////////////////////////////////////////////////
// Focus

TEST_F(PrivateShellProtocolTest, ANoFocusDockNeverTakesTheKeyboardFromAnApplication)
{
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.bindShell());
                XdgWindow *dock = c.makeToplevel("Dock", "cutefish-shell");
                dock->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, dock->surface);
                cutefish_shell_surface_v1_set_role(dock->shellSurface,
                                                   CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK);
                cutefish_shell_surface_v1_set_focus_policy(
                        dock->shellSurface, CUTEFISH_SHELL_SURFACE_V1_FOCUS_POLICY_NO_FOCUS);
                cutefish_shell_surface_v1_set_position(dock->shellSurface, 0, 540);
                c.map(dock, QSize(800, 60));

                check([&] {
                    wm::Window *dockWindow = windowWithRole(wm::WindowRole::Dock);
                    ASSERT_NE(nullptr, dockWindow);
                    EXPECT_FALSE(dockWindow->isActive());
                    // Even an explicit activation is refused.
                    windowManager.activation()->activate(dockWindow);
                    EXPECT_FALSE(dockWindow->isActive());
                });
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                check([&] {
                    wm::Window *active = windowManager.activation()->activeWindow();
                    ASSERT_NE(nullptr, active);
                    EXPECT_EQ(wm::WindowRole::Normal, active->role());
                });
            });
}

TEST_F(PrivateShellProtocolTest, AnOsdNeverTakesTheKeyboardWhateverItsFocusPolicy)
{
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.bindShell());
                XdgWindow *osd = c.makeToplevel("Osd", "cutefish-shell");
                osd->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, osd->surface);
                cutefish_shell_surface_v1_set_role(osd->shellSurface,
                                                   CUTEFISH_SHELL_SURFACE_V1_ROLE_OSD);
                cutefish_shell_surface_v1_set_position(osd->shellSurface, 300, 480);
                c.map(osd, QSize(200, 50));

                cutefish_shell_surface_v1_activate(osd->shellSurface);
                c.sync();
                check([&] {
                    wm::Window *osdWindow = windowWithRole(wm::WindowRole::Osd);
                    ASSERT_NE(nullptr, osdWindow);
                    EXPECT_FALSE(osdWindow->isActive());
                    EXPECT_NE(osdWindow->rootSurface(), seat.focusedSurface());
                    windowManager.activation()->activate(osdWindow);
                    EXPECT_FALSE(osdWindow->isActive());
                });
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                check([&] {
                    wm::Window *active = windowManager.activation()->activeWindow();
                    ASSERT_NE(nullptr, active);
                    EXPECT_EQ(wm::WindowRole::Normal, active->role());
                });
            });
}

TEST_F(PrivateShellProtocolTest, TheLauncherTakesFocusThroughActivate)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindShell());
        XdgWindow *launcher = c.makeToplevel("Launcher", "cutefish-shell");
        launcher->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, launcher->surface);
        cutefish_shell_surface_v1_set_role(launcher->shellSurface,
                                           CUTEFISH_SHELL_SURFACE_V1_ROLE_LAUNCHER);
        cutefish_shell_surface_v1_set_position(launcher->shellSurface, 0, 0);
        c.map(launcher, QSize(800, 600));

        check([&] {
            wm::Window *window = windowWithRole(wm::WindowRole::Launcher);
            ASSERT_NE(nullptr, window);
            windowManager.activation()->activate(nullptr);
            EXPECT_FALSE(window->isActive());
        });

        cutefish_shell_surface_v1_activate(launcher->shellSurface);
        c.sync();
        check([&] {
            wm::Window *window = windowWithRole(wm::WindowRole::Launcher);
            ASSERT_NE(nullptr, window);
            EXPECT_TRUE(window->isActive());
            EXPECT_EQ(window->rootSurface(), seat.focusedSurface());
        });
    });
}

TEST_F(PrivateShellProtocolTest, TheCaptureInterfaceTakesTheKeyboardAndStaysOutOfCaptures)
{
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.bindShell());
                ASSERT_TRUE(awaitRole(wm::WindowRole::Normal));
                XdgWindow *overlay = c.makeToplevel("Capture", "cutefish-shell");
                overlay->shellSurface =
                        cutefish_shell_v1_get_shell_surface(c.shell, overlay->surface);
                cutefish_shell_surface_v1_set_role(overlay->shellSurface,
                                                   CUTEFISH_SHELL_SURFACE_V1_ROLE_CAPTURE);
                cutefish_shell_surface_v1_set_position(overlay->shellSurface, 0, 0);
                c.map(overlay, kOutput.size());
                ASSERT_TRUE(awaitRole(wm::WindowRole::Capture));
                // Mapped, it has the keys: an activate sent before the first
                // commit finds no window yet.
                check([&] {
                    wm::Window *capture = windowWithRole(wm::WindowRole::Capture);
                    ASSERT_NE(nullptr, capture);
                    EXPECT_EQ(capture->rootSurface(), seat.focusedSurface());
                });

                cutefish_shell_surface_v1_activate(overlay->shellSurface);
                c.sync();
                check([&] {
                    wm::Window *capture = windowWithRole(wm::WindowRole::Capture);
                    wm::Window *app = windowWithRole(wm::WindowRole::Normal);
                    ASSERT_NE(nullptr, capture);
                    ASSERT_NE(nullptr, app);
                    // The window being captured keeps looking active.
                    EXPECT_FALSE(capture->isActive());
                    EXPECT_TRUE(app->isActive());
                    EXPECT_EQ(capture->rootSurface(), seat.focusedSurface());
                    EXPECT_TRUE(seat.focusIsBorrowed());
                    EXPECT_EQ(capture, windowManager.windows().back());
                    EXPECT_TRUE(capture->node()->isExcludedFromCapture());
                    EXPECT_FALSE(app->node()->isExcludedFromCapture());
                });

                // Hiding the interface hands the keyboard back.
                wl_surface_attach(overlay->surface, nullptr, 0, 0);
                wl_surface_commit(overlay->surface);
                c.sync();
                check([&] {
                    wm::Window *app = windowWithRole(wm::WindowRole::Normal);
                    ASSERT_NE(nullptr, app);
                    EXPECT_TRUE(app->isActive());
                    EXPECT_EQ(app->rootSurface(), seat.focusedSurface());
                });

                c.map(overlay, kOutput.size());
                cutefish_shell_surface_v1_activate(overlay->shellSurface);
                c.sync();
                check([&] {
                    wm::Window *capture = windowWithRole(wm::WindowRole::Capture);
                    ASSERT_NE(nullptr, capture);
                    EXPECT_EQ(capture->rootSurface(), seat.focusedSurface());
                });

                // A press that passes through it, as while it counts down,
                // takes the keyboard back to the already active window.
                wl_region *none = wl_compositor_create_region(c.compositor);
                wl_surface_set_input_region(overlay->surface, none);
                wl_region_destroy(none);
                wl_surface_commit(overlay->surface);
                c.sync();
                check([&] {
                    wm::Window *app = windowWithRole(wm::WindowRole::Normal);
                    ASSERT_NE(nullptr, app);
                    const QPointF inside = QRectF(app->bounds()).center();
                    windowManager.onPointerButton(std::chrono::steady_clock::now(), inside,
                                                  BTN_LEFT, true);
                    windowManager.onPointerButton(std::chrono::steady_clock::now(), inside,
                                                  BTN_LEFT, false);
                    EXPECT_TRUE(app->isActive());
                    EXPECT_EQ(app->rootSurface(), seat.focusedSurface());
                });

                cutefish_shell_surface_v1_activate(overlay->shellSurface);
                c.sync();

                // So does closing it.
                c.destroyWindowObjects(overlay);
                c.sync();
                check([&] {
                    wm::Window *app = windowWithRole(wm::WindowRole::Normal);
                    ASSERT_NE(nullptr, app);
                    EXPECT_EQ(nullptr, windowWithRole(wm::WindowRole::Capture));
                    EXPECT_TRUE(app->isActive());
                    EXPECT_EQ(app->rootSurface(), seat.focusedSurface());
                });
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
            });
}

////////////////////////////////////////////////////////////////////////////////
// Work area

TEST_F(PrivateShellProtocolTest, ExclusiveZonesCompose)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindShell());
        XdgWindow *statusBar = c.makeToplevel("Status Bar", "cutefish-shell");
        statusBar->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, statusBar->surface);
        cutefish_shell_surface_v1_set_role(statusBar->shellSurface,
                                           CUTEFISH_SHELL_SURFACE_V1_ROLE_PANEL);
        cutefish_shell_surface_v1_set_exclusive_zone(statusBar->shellSurface,
                                                     CUTEFISH_SHELL_SURFACE_V1_EDGE_TOP, 30);
        c.map(statusBar, QSize(800, 30));

        XdgWindow *dock = c.makeToplevel("Dock", "cutefish-shell");
        dock->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, dock->surface);
        cutefish_shell_surface_v1_set_role(dock->shellSurface, CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK);
        cutefish_shell_surface_v1_set_exclusive_zone(dock->shellSurface,
                                                     CUTEFISH_SHELL_SURFACE_V1_EDGE_BOTTOM, 60);
        c.map(dock, QSize(800, 60));
        c.sync();

        check([&] {
            EXPECT_EQ(QRect(0, 30, 800, 510), windowManager.workAreaFor(nullptr));
        });

        // Changing the dock's thickness updates the reservation in place.
        cutefish_shell_surface_v1_set_exclusive_zone(dock->shellSurface,
                                                     CUTEFISH_SHELL_SURFACE_V1_EDGE_BOTTOM, 100);
        c.sync();
        check([&] {
            EXPECT_EQ(QRect(0, 30, 800, 470), windowManager.workAreaFor(nullptr));
        });

        // Auto-hiding the dock gives the space back.
        cutefish_shell_surface_v1_clear_exclusive_zone(dock->shellSurface);
        c.sync();
        check([&] {
            EXPECT_EQ(QRect(0, 30, 800, 570), windowManager.workAreaFor(nullptr));
        });
    });
}

TEST_F(PrivateShellProtocolTest, MaximizedFollowsTheWorkAreaAndFullscreenDoesNot)
{
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.bindShell());
                XdgWindow *statusBar = c.makeToplevel("Status Bar", "cutefish-shell");
                statusBar->shellSurface =
                        cutefish_shell_v1_get_shell_surface(c.shell, statusBar->surface);
                cutefish_shell_surface_v1_set_role(statusBar->shellSurface,
                                                   CUTEFISH_SHELL_SURFACE_V1_ROLE_PANEL);
                cutefish_shell_surface_v1_set_exclusive_zone(
                        statusBar->shellSurface, CUTEFISH_SHELL_SURFACE_V1_EDGE_TOP, 30);
                c.map(statusBar, QSize(800, 30));
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                ASSERT_TRUE(awaitRole(wm::WindowRole::Panel));

                xdg_toplevel_set_maximized(window->toplevel);
                c.sync();
                check([&] {
                    wm::Window *app = nullptr;
                    for (wm::Window *candidate : windowManager.windows()) {
                        if (candidate->role() == wm::WindowRole::Normal)
                            app = candidate;
                    }
                    ASSERT_NE(nullptr, app);
                    EXPECT_EQ(QRect(0, 30, 800, 570), app->bounds());
                });

                xdg_toplevel_set_fullscreen(window->toplevel, nullptr);
                c.sync();
                check([&] {
                    wm::Window *app = nullptr;
                    for (wm::Window *candidate : windowManager.windows()) {
                        if (candidate->role() == wm::WindowRole::Normal)
                            app = candidate;
                    }
                    ASSERT_NE(nullptr, app);
                    EXPECT_EQ(kOutput, app->bounds());
                });
            });
}

////////////////////////////////////////////////////////////////////////////////
// Window management

TEST_F(PrivateShellProtocolTest, TheManagerEnumeratesExistingApplicationWindows)
{
    std::promise<void> appMapped;
    auto appMappedFuture = appMapped.get_future();
    std::promise<void> shellDone;
    auto shellDoneFuture = shellDone.get_future();

    run(
            [&](Client &c) {
                ASSERT_EQ(appMappedFuture.wait_for(std::chrono::seconds(5)),
                          std::future_status::ready);
                ASSERT_TRUE(c.bindManagement());
                c.sync();
                EXPECT_EQ(1, c.initialStateDone);
                std::vector<TrackedWindow *> live = c.liveTracked();
                ASSERT_EQ(1u, live.size());
                EXPECT_EQ("App", live[0]->title);
                EXPECT_EQ("app", live[0]->appId);
                EXPECT_NE(0u, live[0]->pid);
                EXPECT_EQ(0u, live[0]->parentId);
                EXPECT_GT(live[0]->parentCount, 0);
                EXPECT_TRUE(live[0]->active);
                EXPECT_EQ(live[0]->id, c.activeWindowId);
                shellDone.set_value();
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                appMapped.set_value();
                ASSERT_TRUE(c.waitFor([&] {
                    return shellDoneFuture.wait_for(std::chrono::milliseconds(0))
                            == std::future_status::ready;
                }));
            });
}

TEST_F(PrivateShellProtocolTest, DesksAndTheWindowsOnThemReachTheShell)
{
    std::promise<void> appMapped;
    auto appMappedFuture = appMapped.get_future();
    std::promise<void> shellDone;
    auto shellDoneFuture = shellDone.get_future();

    run(
            [&](Client &c) {
                ASSERT_EQ(appMappedFuture.wait_for(std::chrono::seconds(5)),
                          std::future_status::ready);
                ASSERT_TRUE(c.bindManagement());
                c.sync();
                // The desks come before the windows on them.
                ASSERT_EQ(1u, c.desks.size());
                const uint32_t first = c.desks[0];
                EXPECT_NE(0u, first);
                EXPECT_EQ(first, c.activeDesk);
                std::vector<TrackedWindow *> live = c.liveTracked();
                ASSERT_EQ(1u, live.size());
                TrackedWindow *task = live[0];
                EXPECT_EQ(first, task->desk);

                cutefish_window_management_v1_create_desk(c.management, 0);
                c.sync();
                ASSERT_EQ(2u, c.desks.size());
                const uint32_t second = c.desks[1];
                EXPECT_EQ(first, c.activeDesk);
                EXPECT_TRUE(c.deskNames.empty());

                cutefish_window_management_v1_set_desk_name(c.management, second, "Work");
                c.sync();
                EXPECT_EQ("Work", c.deskNames[second]);

                // Sent to the other desk, the window leaves; the user stays.
                cutefish_window_v1_move_to_desk(task->object, second);
                c.sync();
                EXPECT_EQ(second, task->desk);
                EXPECT_EQ(first, c.activeDesk);
                EXPECT_FALSE(task->active);

                // Activating it takes the user to its desk.
                cutefish_window_v1_activate(task->object);
                c.sync();
                EXPECT_EQ(second, c.activeDesk);
                EXPECT_TRUE(task->active);

                // Zero is every desk, and a desk takes it back.
                cutefish_window_v1_move_to_desk(task->object, 0);
                c.sync();
                EXPECT_EQ(0u, task->desk);
                cutefish_window_v1_move_to_desk(task->object, second);
                c.sync();
                EXPECT_EQ(second, task->desk);

                cutefish_window_management_v1_activate_desk(c.management, first);
                c.sync();
                EXPECT_EQ(first, c.activeDesk);

                // A removed desk hands its window to the one on its left.
                cutefish_window_management_v1_remove_desk(c.management, second);
                c.sync();
                ASSERT_EQ(1u, c.desks.size());
                EXPECT_EQ(first, task->desk);
                EXPECT_EQ(0u, c.deskNames.count(second));
                // The only desk stays.
                cutefish_window_management_v1_remove_desk(c.management, first);
                c.sync();
                EXPECT_EQ(1u, c.desks.size());
                shellDone.set_value();
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                appMapped.set_value();
                ASSERT_TRUE(c.waitFor([&] {
                    return shellDoneFuture.wait_for(std::chrono::milliseconds(0))
                            == std::future_status::ready;
                }));
            });
}

TEST_F(PrivateShellProtocolTest, APickLooksThroughTheCaptureInterface)
{
    std::promise<void> appMapped;
    auto appMappedFuture = appMapped.get_future();
    std::promise<void> shellDone;
    auto shellDoneFuture = shellDone.get_future();

    run(
            [&](Client &c) {
                ASSERT_TRUE(c.bindShell());
                const std::tuple<const char *, uint32_t, QPoint, QSize> surfaces[] = {
                        {"Desktop", CUTEFISH_SHELL_SURFACE_V1_ROLE_DESKTOP, QPoint(0, 0),
                         kOutput.size()},
                        {"Dock", CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK, QPoint(200, 540),
                         QSize(400, 50)},
                        {"Capture", CUTEFISH_SHELL_SURFACE_V1_ROLE_CAPTURE, QPoint(0, 0),
                         kOutput.size()},
                };
                for (const auto &[title, role, position, size] : surfaces) {
                    XdgWindow *window = c.makeToplevel(title, "cutefish-shell");
                    window->shellSurface =
                            cutefish_shell_v1_get_shell_surface(c.shell, window->surface);
                    cutefish_shell_surface_v1_set_role(window->shellSurface, role);
                    cutefish_shell_surface_v1_set_position(window->shellSurface, position.x(),
                                                           position.y());
                    c.map(window, size);
                }
                ASSERT_TRUE(awaitRole(wm::WindowRole::Capture));
                ASSERT_EQ(appMappedFuture.wait_for(std::chrono::seconds(5)),
                          std::future_status::ready);
                ASSERT_TRUE(c.bindManagement());
                c.sync();
                std::vector<TrackedWindow *> live = c.liveTracked();
                ASSERT_EQ(1u, live.size());
                const QPoint app = live[0]->geometry.center();
                ASSERT_FALSE(live[0]->geometry.contains(QPoint(790, 10)));

                cutefish_window_management_v1_pick(c.management, 1, app.x(), app.y());
                cutefish_window_management_v1_pick(c.management, 2, 250, 560);
                cutefish_window_management_v1_pick(c.management, 3, 790, 10);
                c.sync();
                ASSERT_EQ(3u, c.picks.size());
                EXPECT_EQ(uint32_t(CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_WINDOW),
                          c.picks[1].kind);
                EXPECT_EQ(live[0]->id, c.picks[1].id);
                EXPECT_EQ(uint32_t(CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_DOCK),
                          c.picks[2].kind);
                EXPECT_EQ(0u, c.picks[2].id);
                EXPECT_EQ(uint32_t(CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_DESKTOP),
                          c.picks[3].kind);
                shellDone.set_value();
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                appMapped.set_value();
                ASSERT_TRUE(c.waitFor([&] {
                    return shellDoneFuture.wait_for(std::chrono::milliseconds(0))
                            == std::future_status::ready;
                }));
            });
}

TEST_F(PrivateShellProtocolTest, ShellRoleWindowsAreNotInTheTaskList)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindShell());
        ASSERT_TRUE(c.bindManagement());
        XdgWindow *dock = c.makeToplevel("Dock", "cutefish-shell");
        dock->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, dock->surface);
        cutefish_shell_surface_v1_set_role(dock->shellSurface, CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK);
        c.map(dock, QSize(800, 60));
        c.sync();
        EXPECT_TRUE(c.liveTracked().empty());
    });
}

TEST_F(PrivateShellProtocolTest, LiveChangesAndCommandsReachTheWindow)
{
    std::promise<void> appMapped;
    auto appMappedFuture = appMapped.get_future();
    std::promise<void> shellDone;
    auto shellDoneFuture = shellDone.get_future();

    run(
            [&](Client &c) {
                ASSERT_EQ(appMappedFuture.wait_for(std::chrono::seconds(5)),
                          std::future_status::ready);
                ASSERT_TRUE(c.bindManagement());
                c.sync();
                std::vector<TrackedWindow *> live = c.liveTracked();
                ASSERT_EQ(1u, live.size());
                TrackedWindow *task = live[0];

                cutefish_window_v1_minimize(task->object);
                c.sync();
                EXPECT_TRUE(task->minimized);

                cutefish_window_v1_restore(task->object);
                c.sync();
                EXPECT_FALSE(task->minimized);

                cutefish_window_v1_toggle_maximize(task->object);
                c.sync();
                EXPECT_TRUE(task->maximized);
                cutefish_window_v1_toggle_maximize(task->object);
                c.sync();
                EXPECT_FALSE(task->maximized);

                cutefish_window_v1_activate(task->object);
                c.sync();
                EXPECT_TRUE(task->active);

                // Hiding drops the focus but keeps the state; a hidden window
                // that is minimized stays minimized underneath. Activating
                // brings it all the way back.
                cutefish_window_v1_hide(task->object);
                c.sync();
                EXPECT_TRUE(task->hidden);
                EXPECT_FALSE(task->active);
                cutefish_window_v1_minimize(task->object);
                c.sync();
                EXPECT_TRUE(task->hidden);
                EXPECT_TRUE(task->minimized);
                EXPECT_TRUE(task->thumbnail.isNull());
                cutefish_window_v1_unhide(task->object);
                c.sync();
                EXPECT_FALSE(task->hidden);
                EXPECT_TRUE(task->minimized);
                EXPECT_FALSE(task->active);
                cutefish_window_v1_hide(task->object);
                cutefish_window_v1_activate(task->object);
                c.sync();
                EXPECT_FALSE(task->hidden);
                EXPECT_FALSE(task->minimized);
                EXPECT_TRUE(task->active);

                // The application menu's Hide and Hide Others act on whole
                // applications; Hide Others keeps the one it is given.
                cutefish_window_management_v1_hide_other_applications(c.management, task->object);
                c.sync();
                EXPECT_FALSE(task->hidden);
                cutefish_window_management_v1_hide_application(c.management, task->object);
                c.sync();
                EXPECT_TRUE(task->hidden);
                cutefish_window_v1_activate(task->object);
                c.sync();
                cutefish_window_management_v1_hide_other_applications(c.management, nullptr);
                c.sync();
                EXPECT_TRUE(task->hidden);
                cutefish_window_v1_activate(task->object);
                c.sync();
                EXPECT_FALSE(task->hidden);

                // Quit asks the application's windows to close; it never
                // terminates the compositor's own process, which these
                // in-process clients share.
                cutefish_window_management_v1_quit_application(c.management, task->object);
                c.sync();

                // A window this client does not own still accepts the trusted
                // command; the application decides what close means.
                cutefish_window_v1_close(task->object);
                c.sync();

                cutefish_window_v1_start_move(task->object);
                c.sync();

                shellDone.set_value();
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                appMapped.set_value();
                ASSERT_TRUE(c.waitFor([&] {
                    return shellDoneFuture.wait_for(std::chrono::milliseconds(0))
                            == std::future_status::ready;
                }));
                ASSERT_TRUE(c.waitFor([&] {
                    return window->closes > 1;
                }));
                EXPECT_EQ(2, window->closes);

                // Retitling reaches the shell as a live change.
                xdg_toplevel_set_title(window->toplevel, "Renamed");
                c.sync();
            });
}

TEST_F(PrivateShellProtocolTest, AMinimizedWindowSendsItsThumbnailToEveryManager)
{
    std::promise<void> appMapped;
    auto appMappedFuture = appMapped.get_future();
    std::promise<void> shellDone;
    auto shellDoneFuture = shellDone.get_future();

    QImage snapshot(6, 4, QImage::Format_ARGB32_Premultiplied);
    snapshot.fill(Qt::transparent);
    snapshot.setPixelColor(0, 0, Qt::red);
    snapshot.setPixelColor(5, 3, Qt::blue);

    run(
            [&](Client &c) {
                ASSERT_EQ(appMappedFuture.wait_for(std::chrono::seconds(5)),
                          std::future_status::ready);
                ASSERT_TRUE(c.bindManagement());
                std::vector<TrackedWindow *> live = c.liveTracked();
                ASSERT_EQ(1u, live.size());
                TrackedWindow *task = live[0];

                cutefish_window_v1_minimize(task->object);
                c.sync();
                ASSERT_TRUE(task->minimized);
                EXPECT_TRUE(task->thumbnail.isNull());

                // What rendering hands over once the funnel has its last frame.
                const int done = task->doneCount;
                check([&] {
                    windowManager.windowById(task->id)->setMinimizedSnapshot(snapshot);
                });
                c.sync();
                EXPECT_EQ(task->thumbnail, snapshot);
                EXPECT_EQ(done + 1, task->doneCount);

                // A restarted shell gets it with the initial enumeration.
                const size_t before = c.tracked.size();
                ASSERT_TRUE(c.bindManagement());
                ASSERT_EQ(before + 1, c.tracked.size());
                EXPECT_TRUE(c.tracked.back()->minimized);
                EXPECT_EQ(c.tracked.back()->thumbnail, snapshot);

                cutefish_window_v1_restore(task->object);
                c.sync();
                EXPECT_FALSE(task->minimized);
                check([&] {
                    EXPECT_TRUE(windowManager.windowById(task->id)->minimizedSnapshot().isNull());
                });

                shellDone.set_value();
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                appMapped.set_value();
                ASSERT_TRUE(c.waitFor([&] {
                    return shellDoneFuture.wait_for(std::chrono::milliseconds(0))
                            == std::future_status::ready;
                }));
            });
}

TEST_F(PrivateShellProtocolTest, ClosingAnApplicationWindowClosesItsTask)
{
    std::promise<void> appMapped;
    auto appMappedFuture = appMapped.get_future();
    std::promise<void> managerReady;
    auto managerReadyFuture = managerReady.get_future();
    std::promise<void> shellDone;
    auto shellDoneFuture = shellDone.get_future();

    run(
            [&](Client &c) {
                ASSERT_EQ(appMappedFuture.wait_for(std::chrono::seconds(5)),
                          std::future_status::ready);
                ASSERT_TRUE(c.bindManagement());
                c.sync();
                ASSERT_EQ(1u, c.liveTracked().size());
                managerReady.set_value();

                ASSERT_TRUE(c.waitFor([&] {
                    return c.liveTracked().empty();
                }));
                EXPECT_TRUE(c.liveTracked().empty());
                EXPECT_EQ(0u, c.activeWindowId);
                shellDone.set_value();
            },
            [&](Client &c) {
                XdgWindow *window = c.makeToplevel("App", "app");
                c.map(window, QSize(300, 200));
                appMapped.set_value();
                ASSERT_EQ(managerReadyFuture.wait_for(std::chrono::seconds(5)),
                          std::future_status::ready);
                c.destroyWindowObjects(window);
                c.sync();
                ASSERT_TRUE(c.waitFor([&] {
                    return shellDoneFuture.wait_for(std::chrono::milliseconds(0))
                            == std::future_status::ready;
                }));
            });
}

////////////////////////////////////////////////////////////////////////////////
// Shell lifetime

TEST_F(PrivateShellProtocolTest, ShellDisconnectClearsItsReservationsAndKeepsApplicationsAlive)
{
    auto privileged = std::make_unique<wayland::PrivilegedClients>(server.display());
    auto waylandSeat = std::make_unique<wayland::WaylandSeat>(&seat, server.serialTracker());
    auto xdgShell = std::make_unique<wayland::WaylandXdgShell>(&compositor, &windowManager, &seat,
                                                               server.serialTracker());
    auto cutefishShell =
            std::make_unique<wayland::WaylandCutefishShell>(&windowManager, privileged.get());
    auto management = std::make_unique<wayland::WaylandCutefishWindowManagement>(&windowManager,
                                                                                 privileged.get());

    ASSERT_TRUE(wayland::createCompositorGlobal(server.display(), &compositor));
    ASSERT_TRUE(wayland::createSubcompositorGlobal(server.display()));
    ASSERT_TRUE(wayland::createShmGlobal(server.display(), &compositor));
    ASSERT_TRUE(wayland::createSeatGlobal(server.display(), waylandSeat.get()));
    ASSERT_TRUE(wayland::createXdgShellGlobal(server.display(), xdgShell.get()));
    ASSERT_TRUE(wayland::createCutefishShellGlobal(server.display(), cutefishShell.get()));
    ASSERT_TRUE(wayland::createCutefishWindowManagementGlobal(server.display(), management.get()));
    privileged->setShellDisconnectedCallback([this] {
        windowManager.clearAllExclusiveZones();
    });

    // The application outlives both shell generations.
    int appFds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, appFds));
    ASSERT_TRUE(wl_client_create(server.display(), appFds[0]));

    std::promise<void> appMapped;
    auto appMappedFuture = appMapped.get_future();
    std::promise<void> finished;
    auto finishedFuture = finished.get_future();

    std::atomic<int> running = 2;
    std::thread appThread([&] {
        RunningClient runningClient {running};
        {
            Client c(appFds[1]);
            XdgWindow *window = c.makeToplevel("App", "app");
            c.map(window, QSize(300, 200));
            appMapped.set_value();
            ASSERT_TRUE(c.waitFor([&] {
                return finishedFuture.wait_for(std::chrono::milliseconds(0))
                        == std::future_status::ready;
            }));
        }
    });

    std::thread driver([&] {
        RunningClient runningClient {running};
        ASSERT_EQ(appMappedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);

        // First shell generation: reserve work area, then disconnect.
        int firstFds[2];
        ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, firstFds));
        check([&] {
            ASSERT_TRUE(privileged->adoptShellConnection(firstFds[0]));
        });

        cutefish_shell_surface_v1 *zone = nullptr;
        {
            Client c(firstFds[1]);
            ASSERT_TRUE(c.bindShell());
            ASSERT_TRUE(c.bindManagement());
            c.sync();
            EXPECT_EQ(1u, c.liveTracked().size());

            XdgWindow *bar = c.makeToplevel("Status Bar", "cutefish-shell");
            bar->shellSurface = cutefish_shell_v1_get_shell_surface(c.shell, bar->surface);
            cutefish_shell_surface_v1_set_role(bar->shellSurface,
                                               CUTEFISH_SHELL_SURFACE_V1_ROLE_PANEL);
            cutefish_shell_surface_v1_set_exclusive_zone(bar->shellSurface,
                                                         CUTEFISH_SHELL_SURFACE_V1_EDGE_TOP, 30);
            c.map(bar, QSize(800, 30));
            c.sync();
            check([&] {
                EXPECT_EQ(QRect(0, 30, 800, 570), windowManager.workAreaFor(nullptr));
            });
            zone = bar->shellSurface;
        }
        (void)zone;

        // Closing the socket is asynchronous: the compositor notices only when
        // it next dispatches. Poll until the destroy listener has run.
        bool gone = false;
        const auto destroyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!gone && std::chrono::steady_clock::now() < destroyDeadline) {
            check([&] {
                gone = privileged->shellClient() == nullptr;
            });
        }
        EXPECT_TRUE(gone);

        check([&] {
            EXPECT_EQ(kOutput, windowManager.workAreaFor(nullptr));
            // Only the application's window is left.
            ASSERT_EQ(1u, windowManager.windows().size());
            EXPECT_EQ(wm::WindowRole::Normal, windowManager.windows().front()->role());
        });

        // Second generation: a fresh connection re-enumerates everything.
        int secondFds[2];
        ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, secondFds));
        check([&] {
            ASSERT_TRUE(privileged->adoptShellConnection(secondFds[0]));
        });
        {
            Client c(secondFds[1]);
            ASSERT_TRUE(c.bindManagement());
            c.sync();
            EXPECT_EQ(1, c.initialStateDone);
            ASSERT_EQ(1u, c.liveTracked().size());
            EXPECT_EQ("App", c.liveTracked()[0]->title);
        }

        finished.set_value();
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (running > 0) {
        server.dispatch(std::chrono::milliseconds(1));
        server.flush();
        std::function<void()> work;
        {
            std::lock_guard lock(mutex);
            if (!pendingChecks.empty()) {
                work = std::move(pendingChecks.front());
                pendingChecks.pop_front();
            }
        }
        if (work)
            work();
        if (std::chrono::steady_clock::now() > deadline) {
            ADD_FAILURE() << "timed out";
            break;
        }
    }
    driver.join();
    appThread.join();
    wl_display_destroy_clients(server.display());
}

TEST_F(PrivateShellProtocolTest, PerSurfaceMenusReachShellBeforeMapAndOnReplacement)
{
    run(
            [&](Client &c) {
                ASSERT_TRUE(c.globals.contains("org_kde_kwin_appmenu_manager"));
                EXPECT_FALSE(c.globals.contains("gtk_shell1"));
                auto *menus = static_cast<org_kde_kwin_appmenu_manager *>(wl_registry_bind(
                        c.registry, c.globals.at("org_kde_kwin_appmenu_manager").first,
                        &org_kde_kwin_appmenu_manager_interface, 2));
                auto *first = c.makeToplevel("First", "org.test.App");
                auto *second = c.makeToplevel("Second", "org.test.App");
                auto *firstMenu = org_kde_kwin_appmenu_manager_create(menus, first->surface);
                auto *secondMenu = org_kde_kwin_appmenu_manager_create(menus, second->surface);
                org_kde_kwin_appmenu_set_address(firstMenu, ":1.123", "/first");
                org_kde_kwin_appmenu_set_address(secondMenu, ":1.123", "/second");
                c.map(first, QSize(180, 120));
                c.map(second, QSize(180, 120));
                ASSERT_TRUE(c.bindManagement());
                ASSERT_EQ(c.liveTracked().size(), 2u);
                TrackedWindow *a = nullptr;
                TrackedWindow *b = nullptr;
                for (auto *window : c.liveTracked()) {
                    if (window->title == "First")
                        a = window;
                    if (window->title == "Second")
                        b = window;
                }
                ASSERT_TRUE(a);
                ASSERT_TRUE(b);
                EXPECT_EQ(a->pid, b->pid);
                EXPECT_EQ(a->menuService, ":1.123");
                EXPECT_EQ(a->menuPath, "/first");
                EXPECT_EQ(b->menuPath, "/second");
                int done = a->doneCount;
                org_kde_kwin_appmenu_set_address(firstMenu, ":1.124", "/replacement");
                c.sync();
                EXPECT_EQ(a->menuService, ":1.124");
                EXPECT_EQ(a->menuPath, "/replacement");
                EXPECT_GT(a->doneCount, done);
                EXPECT_EQ(b->menuPath, "/second");
                auto *duplicate = org_kde_kwin_appmenu_manager_create(menus, first->surface);
                org_kde_kwin_appmenu_set_address(duplicate, ":1.125", "/newest");
                c.sync();
                EXPECT_EQ(a->menuPath, "/newest");
                org_kde_kwin_appmenu_release(duplicate);
                org_kde_kwin_appmenu_manager_release(menus);
                c.sync();
                EXPECT_EQ(a->menuPath, "/replacement");
                org_kde_kwin_appmenu_release(firstMenu);
                c.sync();
                EXPECT_TRUE(a->menuService.empty());
                EXPECT_TRUE(a->menuPath.empty());
                c.destroyWindowObjects(second);
                c.sync();
                org_kde_kwin_appmenu_set_address(secondMenu, ":1.123", "/inert");
                org_kde_kwin_appmenu_release(secondMenu);
                EXPECT_GE(c.roundtrip(), 0);
                EXPECT_TRUE(b->closed);
            },
            [](Client &app) {
                EXPECT_TRUE(app.globals.contains("org_kde_kwin_appmenu_manager"));
                EXPECT_FALSE(app.globals.contains("cutefish_window_management_v1"));
            });
}

TEST_F(PrivateShellProtocolTest, AppMenuVersionOneValidatesAndClearsAddresses)
{
    run([](Client &c) {
        auto *manager = static_cast<org_kde_kwin_appmenu_manager *>(
                wl_registry_bind(c.registry, c.globals.at("org_kde_kwin_appmenu_manager").first,
                                 &org_kde_kwin_appmenu_manager_interface, 1));
        auto *window = c.makeToplevel("Menu", "org.test.App");
        auto *menu = org_kde_kwin_appmenu_manager_create(manager, window->surface);
        org_kde_kwin_appmenu_manager_destroy(manager);
        org_kde_kwin_appmenu_set_address(menu, "org.test.App", "/");
        c.map(window, QSize(180, 120));
        ASSERT_TRUE(c.bindManagement());
        ASSERT_EQ(c.liveTracked().size(), 1u);
        auto *tracked = c.liveTracked().front();
        EXPECT_EQ(tracked->menuService, "org.test.App");
        EXPECT_EQ(tracked->menuPath, "/");
        for (const auto &address :
             std::vector<std::pair<const char *, const char *>> {{"", ""},
                                                                 {"invalid", "/menu"},
                                                                 {"1.invalid", "/menu"},
                                                                 {"org.test.App", "invalid"},
                                                                 {"org.test.App", "/menu/"},
                                                                 {"org.test.App", "/bad-path"}}) {
            org_kde_kwin_appmenu_set_address(menu, address.first, address.second);
            c.sync();
            EXPECT_TRUE(tracked->menuService.empty());
            EXPECT_TRUE(tracked->menuPath.empty());
            org_kde_kwin_appmenu_set_address(menu, ":1.42", "/menu");
            c.sync();
            EXPECT_EQ(tracked->menuService, ":1.42");
            EXPECT_EQ(tracked->menuPath, "/menu");
        }
        org_kde_kwin_appmenu_release(menu);
        c.sync();
        EXPECT_TRUE(tracked->menuPath.empty());
    });
}

TEST_F(PrivateShellProtocolTest, ActiveSurfaceCanBeDestroyedBeforeItsMenuResource)
{
    run([](Client &c) {
        auto *manager = static_cast<org_kde_kwin_appmenu_manager *>(
                wl_registry_bind(c.registry, c.globals.at("org_kde_kwin_appmenu_manager").first,
                                 &org_kde_kwin_appmenu_manager_interface, 2));
        auto *window = c.makeToplevel("Menu", "org.test.App");
        auto *menu = org_kde_kwin_appmenu_manager_create(manager, window->surface);
        org_kde_kwin_appmenu_set_address(menu, ":1.42", "/menu");
        c.map(window, QSize(180, 120));
        ASSERT_TRUE(c.bindManagement());
        ASSERT_EQ(c.liveTracked().size(), 1u);
        auto *tracked = c.liveTracked().front();
        cutefish_window_v1_activate(tracked->object);
        c.sync();
        ASSERT_EQ(c.activeWindowId, tracked->id);
        wl_surface_destroy(window->surface);
        window->surface = nullptr;
        ASSERT_GE(c.roundtrip(), 0);
        EXPECT_TRUE(tracked->closed);
        EXPECT_EQ(c.activeWindowId, 0u);
        org_kde_kwin_appmenu_set_address(menu, ":1.42", "/inert");
        org_kde_kwin_appmenu_release(menu);
        org_kde_kwin_appmenu_manager_release(manager);
        EXPECT_GE(c.roundtrip(), 0);
    });
}

} // namespace

TEST_F(PrivateShellProtocolTest, ManagedPopupsAreNotAnnouncedAsTasks)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindManagement());
        check([&] {
            wm::WindowInit init;
            init.type = wm::WindowType::Popup;
            init.requestedSize = QSize(120, 90);
            auto *popup = windowManager.createWindow(nullptr, init);
            EXPECT_FALSE(windowManager.activation()->canActivate(popup));
            windowManager.destroyWindow(popup);
        });
        c.sync();
        EXPECT_TRUE(c.tracked.empty());
        EXPECT_EQ(0u, c.activeWindowId);
    });
}

TEST_F(PrivateShellProtocolTest, WindowHintsApplyBeforeMapAndUpdateWithoutChangingFocus)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindManagement());
        ASSERT_TRUE(c.globals.contains("cutefish_window_hints_v1"));
        auto *hints = static_cast<cutefish_window_hints_v1 *>(wl_registry_bind(
                c.registry, c.globals["cutefish_window_hints_v1"].first,
                &cutefish_window_hints_v1_interface, 1));
        auto *window = c.makeToplevel("Helper", "arbitrary-app-id");
        cutefish_window_hints_v1_set_skip_taskbar(hints, window->surface, 1);
        c.map(window, QSize(300, 200));
        ASSERT_EQ(c.liveTracked().size(), 1u);
        auto *tracked = c.liveTracked().front();
        EXPECT_TRUE(tracked->skipTaskbar);
        EXPECT_GT(tracked->skipTaskbarCount, 0);
        EXPECT_EQ(c.activeWindowId, tracked->id);
        check([&] {
            auto *model = windowManager.windowById(tracked->id);
            ASSERT_TRUE(model);
            EXPECT_TRUE(model->skipTaskbar());
            EXPECT_FALSE(wm::canIncludeWindowInMruList(model));
        });

        cutefish_window_hints_v1_set_skip_taskbar(hints, window->surface, 0);
        c.sync();
        EXPECT_FALSE(tracked->skipTaskbar);
        EXPECT_EQ(c.activeWindowId, tracked->id);
        check([&] {
            EXPECT_TRUE(wm::canIncludeWindowInMruList(windowManager.windowById(tracked->id)));
        });
        cutefish_window_hints_v1_set_skip_taskbar(hints, window->surface, 1);
        cutefish_window_hints_v1_destroy(hints);
        c.sync();
        EXPECT_TRUE(tracked->skipTaskbar);

        cutefish_window_management_v1_destroy(c.management);
        c.management = nullptr;
        const size_t oldCount = c.tracked.size();
        ASSERT_TRUE(c.bindManagement());
        ASSERT_GT(c.tracked.size(), oldCount);
        EXPECT_TRUE(c.tracked.back()->skipTaskbar);
    });
}

TEST_F(PrivateShellProtocolTest, ShellApplicationWindowsStayOutOfManagementAndPreserveContext)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindManagement());
        auto *app = c.makeToplevel("App", "ordinary-app");
        c.map(app, QSize(300, 200));
        ASSERT_EQ(c.liveTracked().size(), 1u);
        const quint64 appId = c.liveTracked().front()->id;
        EXPECT_EQ(c.activeWindowId, appId);

        auto *helper = c.makeToplevel("Properties", "cutefish-shell");
        c.map(helper, QSize(200, 100));
        ASSERT_EQ(c.liveTracked().size(), 1u);
        EXPECT_EQ(c.activeWindowId, appId);
        check([&] {
            auto *active = windowManager.activation()->activeWindow();
            ASSERT_TRUE(active);
            EXPECT_TRUE(active->isShellApplication());
            EXPECT_FALSE(active->isShellWindow());
            EXPECT_TRUE(active->skipTaskbar());
            EXPECT_FALSE(wm::canIncludeWindowInMruList(active));
        });

        cutefish_window_management_v1_destroy(c.management);
        c.management = nullptr;
        const size_t before = c.tracked.size();
        ASSERT_TRUE(c.bindManagement());
        ASSERT_EQ(c.tracked.size(), before + 1);
        EXPECT_EQ(c.tracked.back()->id, appId);
        EXPECT_EQ(c.activeWindowId, appId);

        c.destroyWindowObjects(app);
        c.sync();
        EXPECT_TRUE(c.tracked.back()->closed);
        EXPECT_EQ(c.activeWindowId, 0u);
    });
}

TEST_F(PrivateShellProtocolTest, ChangingTheShellAppIdClosesAndReannouncesTheTask)
{
    run([&](Client &c) {
        ASSERT_TRUE(c.bindManagement());
        auto *app = c.makeToplevel("App", "ordinary-app");
        c.map(app, QSize(300, 200));
        const quint64 appId = c.liveTracked().front()->id;
        auto *helper = c.makeToplevel("Helper", "cutefish-shell");
        c.map(helper, QSize(200, 100));
        ASSERT_EQ(c.liveTracked().size(), 1u);

        xdg_toplevel_set_app_id(helper->toplevel, "other-application");
        c.sync();
        ASSERT_EQ(c.liveTracked().size(), 2u);
        auto *task = c.tracked.back().get();
        const quint64 helperId = task->id;
        EXPECT_FALSE(task->skipTaskbar);
        EXPECT_EQ(c.activeWindowId, helperId);
        check([&] {
            EXPECT_TRUE(wm::canIncludeWindowInMruList(windowManager.windowById(helperId)));
        });

        xdg_toplevel_set_app_id(helper->toplevel, "cutefish-shell");
        c.sync();
        EXPECT_TRUE(task->closed);
        ASSERT_EQ(c.liveTracked().size(), 1u);
        EXPECT_EQ(c.activeWindowId, appId);

        xdg_toplevel_set_app_id(helper->toplevel, "other-application");
        c.sync();
        ASSERT_EQ(c.liveTracked().size(), 2u);
        EXPECT_EQ(c.tracked.back()->id, helperId);
        EXPECT_EQ(c.activeWindowId, helperId);
    });
}
