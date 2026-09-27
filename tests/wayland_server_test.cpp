// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A Cutefish stand-in for chromium/src/components/exo/wayland/test/
// wayland_server_test_base.*: a real libwayland client on its own thread,
// driven by a Server running on the Qt event loop of the main thread.
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#include "wayland/server.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <wayland-client.h>

#include <atomic>
#include <thread>

namespace cutefish::wayland {
namespace {

// Runs a wl_display connection on its own thread so that the server can keep
// dispatching on the main thread's Qt event loop.
class TestClient
{
public:
    void start(const QString& socketName)
    {
        m_thread = std::thread([this, name = socketName.toStdString()] { run(name); });
    }

    void requestDisconnect() { m_shouldDisconnect = true; }

    void join()
    {
        m_shouldDisconnect = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    bool connected() const { return m_connected; }
    bool roundtripDone() const { return m_roundtripDone; }
    bool finished() const { return m_finished; }
    int globalCount() const { return m_globalCount; }

private:
    void run(const std::string& socketName)
    {
        wl_display* display = wl_display_connect(socketName.c_str());
        if (!display) {
            m_finished = true;
            return;
        }
        m_connected = true;

        static const wl_registry_listener listener = {
            [](void* data, wl_registry*, uint32_t, const char*, uint32_t) {
                ++*static_cast<std::atomic<int>*>(data);
            },
            [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry* registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &listener, &m_globalCount);

        // Blocks until the server answers, which only happens if the Qt
        // integration is dispatching and flushing.
        const bool ok = wl_display_roundtrip(display) >= 0;
        m_roundtripDone = ok;

        while (!m_shouldDisconnect)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        m_finished = true;
    }

    std::thread m_thread;
    std::atomic<bool> m_connected { false };
    std::atomic<bool> m_roundtripDone { false };
    std::atomic<bool> m_shouldDisconnect { false };
    std::atomic<bool> m_finished { false };
    std::atomic<int> m_globalCount { 0 };
};

class WaylandServerTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_runtimeDir.isValid());
        qputenv("XDG_RUNTIME_DIR", m_runtimeDir.path().toUtf8());

        static int argc = 1;
        static char arg0[] = "wayland_server_test";
        static char* argv[] = { arg0, nullptr };
        if (!QCoreApplication::instance())
            new QCoreApplication(argc, argv);

        m_server = std::make_unique<Server>();
        ASSERT_TRUE(m_server->display());
    }

    void TearDown() override { m_server.reset(); }

    // Spins the Qt event loop, which is what drives the server, until
    // `predicate` holds.
    template<class Predicate>
    bool waitFor(Predicate predicate, int timeoutMs = 5000)
    {
        QDeadlineTimer deadline(timeoutMs);
        while (!predicate()) {
            if (deadline.hasExpired())
                return false;
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 10);
        }
        return true;
    }

    QTemporaryDir m_runtimeDir;
    std::unique_ptr<Server> m_server;
};

TEST_F(WaylandServerTest, AddSocketAutoPicksAName)
{
    EXPECT_TRUE(m_server->socketName().isEmpty());
    ASSERT_TRUE(m_server->addSocket());
    EXPECT_TRUE(m_server->socketName().startsWith(QLatin1String("wayland-")));
}

TEST_F(WaylandServerTest, AddSocketHonoursAnExplicitName)
{
    ASSERT_TRUE(m_server->addSocket(QStringLiteral("cutefish-test-0")));
    EXPECT_EQ(QStringLiteral("cutefish-test-0"), m_server->socketName());
}

TEST_F(WaylandServerTest, EventLoopFdIsExposedOnlyWithADisplay)
{
    EXPECT_GE(m_server->fileDescriptor(), 0);
    EXPECT_TRUE(m_server->dispatch(std::chrono::milliseconds(0)));
}

TEST_F(WaylandServerTest, ServerForDisplayResolvesBackToTheServer)
{
    EXPECT_EQ(m_server.get(), Server::serverForDisplay(m_server->display()));
    EXPECT_EQ(nullptr, Server::serverForDisplay(nullptr));
}

TEST_F(WaylandServerTest, ClientConnectsAndSeesAnEmptyRegistry)
{
    ASSERT_TRUE(m_server->addSocket());
    ASSERT_TRUE(m_server->start());

    TestClient client;
    client.start(m_server->socketName());

    ASSERT_TRUE(waitFor([&] { return client.roundtripDone(); }));
    EXPECT_TRUE(client.connected());
    EXPECT_EQ(1, m_server->trackedClientCount());
    // A bare server registers no globals.
    EXPECT_EQ(0, client.globalCount());

    client.requestDisconnect();
    ASSERT_TRUE(waitFor([&] { return client.finished(); }));

    // The server notices the disconnect once it dispatches again.
    ASSERT_TRUE(waitFor([&] { return m_server->trackedClientCount() == 0; }));
    client.join();
}

TEST_F(WaylandServerTest, ServerShutsDownWithAClientStillConnected)
{
    ASSERT_TRUE(m_server->addSocket());
    ASSERT_TRUE(m_server->start());

    TestClient client;
    client.start(m_server->socketName());
    ASSERT_TRUE(waitFor([&] { return client.roundtripDone(); }));
    EXPECT_EQ(1, m_server->trackedClientCount());

    // Destroying the server tears the client down from the server side.
    m_server.reset();

    client.requestDisconnect();
    ASSERT_TRUE(waitFor([&] { return client.finished(); }));
    client.join();
}

} // namespace
} // namespace cutefish::wayland
