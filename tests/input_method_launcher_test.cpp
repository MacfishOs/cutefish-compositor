// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "clipboard_test_util.h"
#include "core/process_environment.h"
#include "input/input_method_settings.h"
#include "wayland/input_method_launcher.h"
#include "wayland/privileged_client.h"
#include "wayland/protocols/input_method.h"

using namespace clipboard_test;
using cutefish::input::findInputMethodProviders;
using cutefish::input::InputMethodSettings;
using cutefish::input::parseDesktopExec;

namespace {
void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

QByteArray provider(const QByteArray &exec, const QByteArray &extra = {})
{
    return "[Desktop Entry]\nType=Application\nName=IME\nName[zh_CN]=输入法\nExec=" + exec
            + "\nX-KDE-Wayland-VirtualKeyboard=true\n" + extra + "[Desktop Action x]\nExec=no\n";
}

QStringList lines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

struct LauncherHarness : Harness {
    wayland::PrivilegedClients privileged {server.display()};
    scene::Node popupLayer;
    wayland::InputMethodManager manager {server.display(), &waylandSeat, &popupLayer};
    wayland::InputMethodLauncher launcher {server.display()};
    QTemporaryDir directory;
    QString log = directory.filePath(QStringLiteral("log"));

    LauncherHarness()
    {
        const auto authorize = [this](const wl_client *client) {
            return launcher.isInputMethod(client);
        };
        manager.setAuthorizer(authorize);
        privileged.addRestrictedGlobal(manager.inputMethodGlobal(), authorize);
        launcher.setRestartDelay(10);
    }

    ~LauncherHarness()
    {
        launcher.setCommand({});
        wl_display_destroy_clients(server.display());
        privileged.removeRestrictedGlobal(manager.inputMethodGlobal());
        manager.shutdown();
    }

    QStringList command(const char *mode) const
    {
        return {QStringLiteral(CUTEFISH_LAUNCHED_CLIENT), QString::fromLatin1(mode), log};
    }
};
} // namespace

TEST(InputMethodSettings, ParsesDesktopExec)
{
    EXPECT_EQ(QStringList({"/usr/libexec/fcitx5-wayland-launcher", "--reopen"}),
              parseDesktopExec(QStringLiteral("/usr/libexec/fcitx5-wayland-launcher --reopen")));
    EXPECT_EQ(QStringList({"/opt/My IME/ime", "--x", "a\"b", "100%"}),
              parseDesktopExec(QStringLiteral(R"("/opt/My IME/ime" --x %U "a\\"b" 100%%)")));
    EXPECT_EQ(QStringList({"ime", "a b"}), parseDesktopExec(QStringLiteral(R"(ime "a\sb")")));
    EXPECT_TRUE(parseDesktopExec(QStringLiteral(R"("unterminated)")).isEmpty());
}

TEST(InputMethodSettings, FindsProvidersWithXdgPrecedence)
{
    QTemporaryDir user, system;
    ASSERT_TRUE(user.isValid() && system.isValid());
    writeFile(system.filePath("fcitx.desktop"), provider("sh --flag %f"));
    writeFile(system.filePath("vendor/ime.desktop"), provider("/bin/sh"));
    writeFile(system.filePath("shadowed.desktop"), provider("sh"));
    writeFile(user.filePath("shadowed.desktop"), "[Desktop Entry]\nType=Application\nExec=sh\n");
    writeFile(system.filePath("hidden.desktop"), provider("sh", "Hidden=true\n"));
    writeFile(system.filePath("tryexec.desktop"), provider("sh", "TryExec=/no/such/ime\n"));
    writeFile(system.filePath("missing.desktop"), provider("/no/such/ime"));
    writeFile(system.filePath("plain.desktop"), "[Desktop Entry]\nType=Application\nExec=sh\n");

    const auto providers = findInputMethodProviders({user.path(), system.path()});
    ASSERT_EQ(2, providers.size());
    EXPECT_EQ(QStringLiteral("fcitx.desktop"), providers.at(0).id);
    EXPECT_EQ(QStandardPaths::findExecutable("sh"), providers.at(0).command.first());
    EXPECT_EQ(QStringList({"--flag"}), providers.at(0).command.mid(1));
    EXPECT_EQ(QStringLiteral("vendor-ime.desktop"), providers.at(1).id);
}

TEST(InputMethodSettings, PersistsValidSelectionsOnly)
{
    QTemporaryDir directory, applications;
    ASSERT_TRUE(directory.isValid() && applications.isValid());
    writeFile(applications.filePath("ime.desktop"), provider("/bin/sh -c true"));
    const QString file = directory.filePath("inputmethod.conf");
    {
        InputMethodSettings settings(nullptr, file, {applications.path()});
        EXPECT_TRUE(settings.inputMethod().isEmpty());
        EXPECT_TRUE(settings.command().isEmpty());
        EXPECT_EQ(QStringList({"ime.desktop"}), settings.availableInputMethods());
        QSignalSpy changed(&settings, &InputMethodSettings::inputMethodChanged);
        QSignalSpy command(&settings, &InputMethodSettings::commandChanged);
        EXPECT_FALSE(settings.setInputMethod("other.desktop"));
        EXPECT_TRUE(settings.setInputMethod("ime.desktop"));
        EXPECT_TRUE(settings.setInputMethod("ime.desktop"));
        EXPECT_EQ(1, changed.count());
        EXPECT_EQ(2, command.count());
        EXPECT_EQ(QStringList({"/bin/sh", "-c", "true"}), settings.command());
    }
    InputMethodSettings restored(nullptr, file, {applications.path()});
    EXPECT_EQ(QStringLiteral("ime.desktop"), restored.inputMethod());
    QFile::remove(applications.filePath("ime.desktop"));
    EXPECT_TRUE(restored.inputMethod().isEmpty());
    EXPECT_TRUE(restored.setInputMethod({}));
}

TEST(InputMethodSettings, ExportsTheSessionService)
{
    if (!QDBusConnection::sessionBus().isConnected())
        GTEST_SKIP() << "No session bus";
    QTemporaryDir directory, applications;
    writeFile(applications.filePath("ime.desktop"), provider("/bin/sh"));
    InputMethodSettings settings(nullptr, directory.filePath("inputmethod.conf"),
                                 {applications.path()});
    ASSERT_TRUE(settings.registerService());
    QDBusInterface remote("com.cutefish.InputMethod1", "/com/cutefish/InputMethod1",
                          "com.cutefish.InputMethod1");
    ASSERT_TRUE(remote.isValid());
    QDBusReply<QStringList> available = remote.call("availableInputMethods");
    EXPECT_EQ(QStringList({"ime.desktop"}), available.value());
    QDBusReply<bool> rejected = remote.call("setInputMethod", QStringLiteral("nope.desktop"));
    EXPECT_FALSE(rejected.value());
    QDBusReply<bool> accepted = remote.call("setInputMethod", QStringLiteral("ime.desktop"));
    EXPECT_TRUE(accepted.value());
    EXPECT_EQ(QStringLiteral("ime.desktop"), remote.property("inputMethod").toString());
    settings.setRunning(true);
    EXPECT_TRUE(remote.property("running").toBool());
}

TEST(InputMethodLauncher, OnlyTheLaunchedConnectionIsTheInputMethod)
{
    LauncherHarness h;
    QSignalSpy running(&h.launcher, &wayland::InputMethodLauncher::runningChanged);
    h.launcher.setCommand(h.command("stay"));
    EXPECT_TRUE(h.launcher.isRunning());
    ASSERT_TRUE(h.until([&] {
        return lines(h.log).size() == 1;
    }));
    EXPECT_EQ(QStringList({"ime"}), lines(h.log));

    auto &app = h.addClient();
    EXPECT_FALSE(h.launcher.isInputMethod(app.serverClient));
    bool visible = false;
    auto *registry = wl_display_get_registry(app.display);
    static const wl_registry_listener listener = {
            [](void *data, wl_registry *, uint32_t, const char *interface, uint32_t) {
                if (!strcmp(interface, "zwp_input_method_manager_v2"))
                    *static_cast<bool *>(data) = true;
            },
            [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry, &listener, &visible);
    h.sync(app);
    EXPECT_FALSE(visible);
    wl_registry_destroy(registry);

    h.launcher.setCommand({});
    EXPECT_FALSE(h.launcher.isRunning());
    EXPECT_EQ(2, running.count());
}

TEST(InputMethodLauncher, RestartsALostConnectionWithALimit)
{
    LauncherHarness h;
    h.launcher.setCommand(h.command("exit"));
    ASSERT_TRUE(h.until(
            [&] {
                return lines(h.log).size() == 6 && !h.launcher.isRunning();
            },
            10000));
    h.until(
            [] {
                return false;
            },
            500);
    EXPECT_EQ(6, lines(h.log).size());
    EXPECT_FALSE(h.launcher.isRunning());

    // Selecting the same method again restarts one that gave up.
    h.launcher.setCommand(h.command("stay"));
    ASSERT_TRUE(h.until([&] {
        return lines(h.log).size() == 7;
    }));
    EXPECT_TRUE(h.launcher.isRunning());
}

TEST(InputMethodLauncher, RestartTakesTheCurrentEnvironment)
{
    // How the input method learns an X11 display Xwayland only opened later.
    LauncherHarness h;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("DISPLAY"));
    h.launcher.setEnvironment(environment);
    h.launcher.setCommand(h.command("display"));
    ASSERT_TRUE(h.until([&] {
        return lines(h.log) == QStringList({"-"}) && h.launcher.isRunning();
    }));
    environment.insert(QStringLiteral("DISPLAY"), QStringLiteral(":7"));
    h.launcher.setEnvironment(environment);
    h.launcher.restart();
    ASSERT_TRUE(h.until([&] {
        return lines(h.log) == QStringList({"-", ":7"}) && h.launcher.isRunning();
    }));
}

TEST(InputMethodLauncher, TheCompositorsOwnQtPlatformStaysWithIt)
{
    // The compositor's Qt renders offscreen; an input method's settings window
    // started from it inherited that and opened nowhere.
    qunsetenv("QT_QPA_PLATFORM");
    cutefish::setPrivateEnvironment("QT_QPA_PLATFORM", "offscreen");
    EXPECT_EQ(qgetenv("QT_QPA_PLATFORM"), "offscreen");
    LauncherHarness h;
    h.launcher.setCommand(h.command("platform"));
    ASSERT_TRUE(h.until([&] {
        return lines(h.log) == QStringList({"-"}) && h.launcher.isRunning();
    }));
}

TEST(InputMethodLauncher, MissingProgramDoesNotLeaveAConnection)
{
    LauncherHarness h;
    h.launcher.setCommand({QStringLiteral("/no/such/input-method")});
    ASSERT_TRUE(h.until(
            [&] {
                return !h.launcher.isRunning();
            },
            5000));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
