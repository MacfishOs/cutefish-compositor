// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/auth_controller.h"
#include "fake_backend.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace cutefish::auth;

// What these defend is the property the whole design rests on: the only thing
// that can unlock a session is a backend that said the credential was correct,
// and it can do it once.
class AuthControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void bothModesShareOneController();
    void failureNeverAccepts();
    void failureResetsAndClears();
    void successAcceptsExactlyOnce();
    void successIsNotUndoneByALateFailure();
    void resetAfterSuccessIsRefused();
    void authenticateWhileBusyIsIgnored();
    void loginStartsTheChosenSession();
    void loginOffersTheAccountsInsteadOfATypedName();
    void loginSessionsFollowWaylandSessions();
};

// The login and lock difference is the backend, and nothing else: the same
// controller class, driven the same way, produces the same states.
// The login screen offers every session, prefers Cutefish, and hands the choice
// to the backend with the attempt that may start it.
void AuthControllerTest::loginStartsTheChosenSession()
{
    FakeBackend backend;
    AuthController controller(AuthController::Login, &backend);
    controller.setSessions({{QStringLiteral("gnome"), QStringLiteral("GNOME"), {}, {}},
                            {QStringLiteral("cutefish-wayland"), QStringLiteral("Cutefish"), {}, {}}});
    QCOMPARE(controller.sessionNames(), (QStringList {QStringLiteral("GNOME"), QStringLiteral("Cutefish")}));
    QVERIFY(controller.canSelectSession());
    QCOMPARE(controller.sessionIndex(), 1);

    controller.setSessionIndex(5);
    QCOMPARE(controller.sessionIndex(), 1);
    controller.setSessionIndex(0);
    QCOMPARE(controller.sessionIndex(), 0);

    controller.authenticate(QStringLiteral("hunter2"));
    QCOMPARE(backend.lastSession, QStringLiteral("gnome"));
    // Settled while the attempt is in flight.
    controller.setSessionIndex(1);
    QCOMPARE(controller.sessionIndex(), 0);
}

void AuthControllerTest::loginOffersTheAccountsInsteadOfATypedName()
{
    FakeBackend backend;
    AuthController controller(AuthController::Login, &backend);
    // The backend's last user is preferred wherever it is listed.
    controller.setUsers({QStringLiteral("alice"), QStringLiteral("tester")});
    QCOMPARE(controller.userName(), QStringLiteral("tester"));
    QCOMPARE(controller.userIndex(), 1);
    QVERIFY(controller.canPickUser());
    QVERIFY(!controller.typesUserName());

    controller.setUserIndex(0);
    QCOMPARE(controller.userName(), QStringLiteral("alice"));
    controller.setUserIndex(7);
    QCOMPARE(controller.userName(), QStringLiteral("alice"));

    // A single account is simply chosen.
    controller.setUsers({QStringLiteral("bob")});
    QCOMPARE(controller.userName(), QStringLiteral("bob"));
    QVERIFY(!controller.canPickUser());

    controller.setUsers({});
    QVERIFY(controller.typesUserName());

    FakeBackend lockBackend;
    AuthController lock(AuthController::Lock, &lockBackend);
    QVERIFY(!lock.typesUserName());
    QVERIFY(!lock.canPickUser());
}

void AuthControllerTest::loginSessionsFollowWaylandSessions()
{
    QTemporaryDir directory;
    const auto write = [&](const QString &name, const QByteArray &contents) {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    };
    write(QStringLiteral("plasma.desktop"),
          "[Desktop Entry]\nName=Plasma\nExec=/usr/bin/startplasma-wayland %u\nDesktopNames=KDE\n");
    write(QStringLiteral("hidden.desktop"), "[Desktop Entry]\nName=Hidden\nExec=/bin/true\nHidden=true\n");
    write(QStringLiteral("gone.desktop"),
          "[Desktop Entry]\nName=Gone\nExec=/bin/true\nTryExec=/nonexistent\n");
    const auto sessions = findLoginSessions({directory.path()});
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.first().id, QStringLiteral("plasma"));
    QCOMPARE(sessions.first().command, QStringList {QStringLiteral("/usr/bin/startplasma-wayland")});
    QVERIFY(sessions.first().environment.contains(QStringLiteral("XDG_SESSION_TYPE=wayland")));
    QVERIFY(sessions.first().environment.contains(QStringLiteral("XDG_CURRENT_DESKTOP=KDE")));

    qputenv("CUTEFISH_SESSION_DIRS", directory.path().toUtf8());
    FakeBackend backend;
    AuthController controller(AuthController::Login, &backend);
    QCOMPARE(controller.sessionNames(), QStringList {QStringLiteral("Plasma")});
    QVERIFY(!controller.canSelectSession());
    QCOMPARE(controller.sessionIndex(), 0);
    qunsetenv("CUTEFISH_SESSION_DIRS");
}

void AuthControllerTest::bothModesShareOneController()
{
    for (auto mode : {AuthController::Login, AuthController::Lock}) {
        FakeBackend backend;
        AuthController controller(mode, &backend);
        controller.start();

        QCOMPARE(controller.mode(), mode);
        QCOMPARE(controller.state(), AuthController::Idle);
        QCOMPARE(backend.startCount, 1);

        controller.authenticate(QStringLiteral("hunter2"));
        QCOMPARE(controller.state(), AuthController::Authenticating);
        QCOMPARE(backend.authenticateCount, 1);
        QCOMPARE(backend.lastSecret, QStringLiteral("hunter2"));

        backend.succeed();
        QCOMPARE(controller.state(), AuthController::Succeeded);
        QCOMPARE(backend.acceptCount, 1);
    }
}

void AuthControllerTest::failureNeverAccepts()
{
    FakeBackend backend;
    AuthController controller(AuthController::Lock, &backend);
    QSignalSpy failedSpy(&controller, &AuthController::authenticationFailed);
    QSignalSpy succeededSpy(&controller, &AuthController::authenticationSucceeded);

    for (int attempt = 0; attempt < 5; ++attempt) {
        controller.authenticate(QStringLiteral("wrong"));
        backend.reject();
    }

    QCOMPARE(failedSpy.count(), 5);
    QCOMPARE(succeededSpy.count(), 0);
    // The one that matters: no number of rejections is an unlock.
    QCOMPARE(backend.acceptCount, 0);
}

void AuthControllerTest::failureResetsAndClears()
{
    FakeBackend backend;
    AuthController controller(AuthController::Lock, &backend);

    controller.authenticate(QStringLiteral("wrong"));
    backend.reject(QStringLiteral("Incorrect password"));

    QCOMPARE(controller.state(), AuthController::Failed);
    QCOMPARE(controller.errorMessage(), QStringLiteral("Incorrect password"));
    QVERIFY(!controller.busy());

    controller.reset();

    QCOMPARE(controller.state(), AuthController::Idle);
    QVERIFY(controller.errorMessage().isEmpty());
    QCOMPARE(backend.resetCount, 1);

    // And the screen still works afterwards, which is the part a stuck error
    // state would break.
    controller.authenticate(QStringLiteral("right"));
    backend.succeed();
    QCOMPARE(controller.state(), AuthController::Succeeded);
    QCOMPARE(backend.acceptCount, 1);
}

void AuthControllerTest::successAcceptsExactlyOnce()
{
    FakeBackend backend;
    AuthController controller(AuthController::Lock, &backend);
    QSignalSpy succeededSpy(&controller, &AuthController::authenticationSucceeded);

    controller.authenticate(QStringLiteral("right"));

    // A backend that reports success more than once - a duplicated greetd
    // response, a racing PAM verdict - still unlocks one session.
    backend.succeed();
    backend.succeed();
    backend.succeed();

    QCOMPARE(succeededSpy.count(), 1);
    QCOMPARE(backend.acceptCount, 1);
}

void AuthControllerTest::successIsNotUndoneByALateFailure()
{
    FakeBackend backend;
    AuthController controller(AuthController::Lock, &backend);

    controller.authenticate(QStringLiteral("right"));
    backend.succeed();
    backend.reject();

    QCOMPARE(controller.state(), AuthController::Succeeded);
    QCOMPARE(backend.acceptCount, 1);
}

void AuthControllerTest::resetAfterSuccessIsRefused()
{
    FakeBackend backend;
    AuthController controller(AuthController::Lock, &backend);

    controller.authenticate(QStringLiteral("right"));
    backend.succeed();
    controller.reset();

    QCOMPARE(controller.state(), AuthController::Succeeded);
    QCOMPARE(backend.resetCount, 0);
}

void AuthControllerTest::authenticateWhileBusyIsIgnored()
{
    FakeBackend backend;
    AuthController controller(AuthController::Lock, &backend);

    controller.authenticate(QStringLiteral("first"));
    controller.authenticate(QStringLiteral("second"));

    QCOMPARE(backend.authenticateCount, 1);
    QCOMPARE(backend.lastSecret, QStringLiteral("first"));
}

QTEST_MAIN(AuthControllerTest)
#include "auth_controller_test.moc"
