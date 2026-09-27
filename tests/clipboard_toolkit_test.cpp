// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <signal.h>
#include <sys/stat.h>

#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include "clipboard_test_util.h"
#include "compositor/output.h"
#include "wayland/protocols/data_control.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wl_shm.h"
#include "wayland/protocols/wl_subcompositor.h"
#include "wayland/protocols/xdg_decoration.h"
#include "wayland/protocols/xdg_shell.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace clipboard_test;

namespace {

class Child : public QProcess
{
public:
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

void matrix(bool sourceGtk, bool destinationGtk, bool binary = false, bool persist = false,
            bool urls = false)
{
    QTemporaryDir runtime;
    ASSERT_TRUE(runtime.isValid());
    chmod(runtime.path().toLocal8Bit().constData(), 0700);

    struct RestoreRuntime {
        QByteArray previous = qgetenv("XDG_RUNTIME_DIR");

        ~RestoreRuntime() { qputenv("XDG_RUNTIME_DIR", previous); }
    } restoreRuntime;

    qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
    Harness h;
    std::unique_ptr<wayland::DataControlManager> dataControl;
    if (persist)
        dataControl = std::make_unique<wayland::DataControlManager>(
                h.server.display(), &h.waylandSeat, [](const wl_client *) {
                    return true;
                });
    wm::WindowManager wm(&h.compositor, &h.seat);
    wm.setOutputBounds(QRect(0, 0, 800, 600));
    wayland::WaylandXdgShell xdg(&h.compositor, &wm, &h.seat, h.server.serialTracker());

    struct DestroyClients {
        wl_display *display;

        ~DestroyClients() { wl_display_destroy_clients(display); }
    } cleanup {h.server.display()};

    compositor::Output output;
    output.pixelSize = QSize(800, 600);
    wayland::createSubcompositorGlobal(h.server.display());
    wayland::createShmGlobal(h.server.display(), &h.compositor);
    wayland::createOutputGlobal(h.server.display(), &output);
    wayland::createXdgShellGlobal(h.server.display(), &xdg);
    wayland::createXdgDecorationGlobal(h.server.display());
    ASSERT_TRUE(h.server.addSocket(QStringLiteral("clipboard-test")));
    ASSERT_TRUE(h.server.start());
    Child source, destination, service;
    auto start = [&](Child &process, bool gtk, const QString &title) {
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto *name : {"DISPLAY", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                                 "QT_QPA_PLATFORMTHEME", "QT_QUICK_BACKEND", "LD_PRELOAD"})
            env.remove(QString::fromLatin1(name));
        env.insert("QT_QPA_PLATFORM", "wayland");
        env.insert("GDK_BACKEND", "wayland");
        env.insert("WAYLAND_DISPLAY", h.server.socketName());
        env.insert("XDG_RUNTIME_DIR", runtime.path());
        process.setProcessEnvironment(env);
        process.start(gtk ? QStringLiteral("/usr/bin/python3")
                          : QStringLiteral(QT_CLIPBOARD_CLIENT),
                      gtk ? QStringList {QStringLiteral(GTK_CLIPBOARD_CLIENT), title}
                          : QStringList {title,
                                         urls             ? QStringLiteral("urls")
                                                 : binary ? QStringLiteral("binary")
                                                          : QStringLiteral("text")});
    };
    auto window = [&](const QString &title) -> wm::Window * {
        for (const auto &w : wm.windows())
            if (w->title() == title)
                return w;
        return nullptr;
    };
    start(source, sourceGtk, QStringLiteral("clipboard-source"));
    ASSERT_TRUE(h.until(
            [&] {
                return window("clipboard-source") != nullptr;
            },
            5000))
            << source.readAllStandardError().constData();
    window("clipboard-source")->activate();
    h.seat.onKey(input::Timestamp {}, urls ? 50 : 46, true);
    h.seat.onKey(input::Timestamp {}, urls ? 50 : 46, false);
    if (urls) {
        ASSERT_TRUE(h.until(
                [&] {
                    return source.saw("MENU");
                },
                5000));
        for (int i = 0; i < 20; ++i)
            h.pump();
        h.seat.onKey({}, 108, true);
        h.seat.onKey({}, 108, false);
        h.seat.onKey({}, 28, true);
        h.seat.onKey({}, 28, false);
    }
    ASSERT_TRUE(h.until(
            [&] {
                return source.saw("PUBLISHED") && h.seat.selection()->source();
            },
            5000))
            << source.readAllStandardError().constData();
    if (persist) {
        service.setProcessEnvironment(source.processEnvironment());
        service.start(qEnvironmentVariable("CUTEFISH_CLIPBOARD_TEST_HELPER"));
        ASSERT_TRUE(service.waitForStarted());
        ASSERT_TRUE(h.until(
                [&] {
                    auto selection = h.seat.selection()->source();
                    return selection && selection->supports("application/x-cutefish-clipboard");
                },
                7000))
                << service.readAllStandardError().constData();
        window("clipboard-source")->activate();
        if (urls) {
            h.seat.onKey({}, 25, true);
            h.seat.onKey({}, 25, false);
            ASSERT_TRUE(h.until(
                    [&] {
                        return source.saw("PASS") || source.state() == QProcess::NotRunning;
                    },
                    5000));
            ASSERT_TRUE(source.saw("PASS")) << source.output.constData();
        }
        h.seat.onKey({}, 16, true);
        h.seat.onKey({}, 16, false);
        ASSERT_TRUE(h.until([&] {
            return source.state() == QProcess::NotRunning;
        }));
        EXPECT_EQ(0, source.exitCode());
    }
    start(destination, destinationGtk, QStringLiteral("clipboard-destination"));
    ASSERT_TRUE(h.until(
            [&] {
                return window("clipboard-destination") != nullptr;
            },
            5000))
            << destination.readAllStandardError().constData();
    window("clipboard-destination")->activate();
    h.seat.onKey(input::Timestamp {}, 47, true);
    h.seat.onKey(input::Timestamp {}, 47, false);
    ASSERT_TRUE(h.until(
            [&] {
                return destination.saw("PASS") || destination.state() == QProcess::NotRunning;
            },
            5000));
    EXPECT_TRUE(destination.saw("PASS"))
            << destination.output.constData() << destination.readAllStandardError().constData();
    ASSERT_TRUE(h.until(
            [&] {
                return destination.state() == QProcess::NotRunning;
            },
            5000));
    EXPECT_EQ(QProcess::NormalExit, destination.exitStatus());
    EXPECT_EQ(0, destination.exitCode()) << destination.readAllStandardError().constData();
    if (!persist) {
        ASSERT_TRUE(window("clipboard-source"));
        window("clipboard-source")->activate();
        h.seat.onKey(input::Timestamp {}, 16, true);
        h.seat.onKey(input::Timestamp {}, 16, false);
        ASSERT_TRUE(h.until(
                [&] {
                    return source.state() == QProcess::NotRunning;
                },
                5000));
        EXPECT_EQ(QProcess::NormalExit, source.exitStatus());
        EXPECT_EQ(0, source.exitCode()) << source.readAllStandardError().constData();
    }
    wl_display_destroy_clients(h.server.display());
}

} // namespace

TEST(ClipboardToolkit, QtToGtk)
{
    matrix(false, true);
}

TEST(ClipboardToolkit, GtkToQt)
{
    matrix(true, false);
}

TEST(ClipboardToolkit, QtToQt)
{
    matrix(false, false);
}

TEST(ClipboardToolkit, QtRawBinaryExactBytes)
{
    matrix(false, false, true);
}

TEST(ClipboardToolkit, ServicePersistsQtAndGtkCopiesAfterTheirWindowsExit)
{
    if (qEnvironmentVariableIsEmpty("CUTEFISH_CLIPBOARD_TEST_HELPER"))
        GTEST_SKIP() << "Set CUTEFISH_CLIPBOARD_TEST_HELPER to the standalone clipboard helper";
    matrix(false, true, false, true);
    matrix(true, false, false, true);
    matrix(false, false, true, true);
}

TEST(ClipboardToolkit, ServicePreservesFileUrlsAfterContextMenuCopy)
{
    if (qEnvironmentVariableIsEmpty("CUTEFISH_CLIPBOARD_TEST_HELPER"))
        GTEST_SKIP() << "Set CUTEFISH_CLIPBOARD_TEST_HELPER to the standalone clipboard helper";
    matrix(false, false, false, true, true);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
