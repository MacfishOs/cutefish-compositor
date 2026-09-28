// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "scene/node.h"
#include "session/screen_saver_service.h"
#include "session/session_controller.h"
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDeadlineTimer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

using namespace cutefish;

class ScreenSaverServiceTest : public QObject
{
    Q_OBJECT
private:
    QDBusConnection client = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("screen-saver-test-client"));

    QDBusPendingCall call(const QString &method, const QVariantList &arguments = {},
                          const QString &path = QStringLiteral("/ScreenSaver"),
                          const QDBusConnection &bus = QDBusConnection(QString()))
    {
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
                                                      path,
                                                      QStringLiteral("org.freedesktop.ScreenSaver"),
                                                      method);
        message.setArguments(arguments);
        return (bus.isConnected() ? bus : client).asyncCall(message);
    }

    // The service answers on this thread, so the wait has to keep processing events.
    static QDBusMessage finished(const QDBusPendingCall &pending)
    {
        QDBusPendingCallWatcher watcher(pending);
        QTest::qWaitFor([&watcher] {
            return watcher.isFinished();
        });
        return watcher.reply();
    }

private slots:

    void initTestCase() { QVERIFY(client.isConnected()); }

    void lockReplyWaitsForEveryOutputAndRepeatedRequestsAreIdempotent()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)},
                               {QStringLiteral("two"), QRect(800, 0, 800, 600)}});
        session::ScreenSaverService service(&controller);
        QVERIFY(service.registerService());
        QSignalSpy changed(&service, &session::ScreenSaverService::ActiveChanged);
        QDBusPendingCallWatcher first(call(QStringLiteral("Lock")));
        QTRY_COMPARE(controller.state(), session::SessionState::Locking);
        QVERIFY(controller.isBlocked());
        QVERIFY(root.isVisible());
        QVERIFY(!first.isFinished());
        QDBusPendingCallWatcher second(call(QStringLiteral("Lock")));
        QDBusPendingCallWatcher active(call(QStringLiteral("GetActive")));
        QTRY_VERIFY(active.isFinished());
        QVERIFY(QDBusPendingReply<bool>(active).value());
        controller.notifyOutputPresented(QStringLiteral("one"));
        QCoreApplication::processEvents();
        QVERIFY(!first.isFinished());
        QVERIFY(!second.isFinished());
        controller.notifyOutputPresented(QStringLiteral("two"));
        QTRY_VERIFY(first.isFinished());
        QTRY_VERIFY(second.isFinished());
        QVERIFY(!first.isError());
        QVERIFY(!second.isError());
        QCOMPARE(controller.state(), session::SessionState::Locked);
        QCOMPARE(changed.size(), 1);
        QDBusPendingCallWatcher repeated(call(QStringLiteral("Lock")));
        QTRY_VERIFY(repeated.isFinished());
        QVERIFY(!repeated.isError());
        QCOMPARE(changed.size(), 1);
        QVERIFY(controller.requestUnlock());
        QVERIFY(service.GetActive());
        QDBusPendingCallWatcher duringUnlock(call(QStringLiteral("Lock")));
        QTRY_VERIFY(duringUnlock.isFinished());
        QVERIFY(duringUnlock.isError());
        controller.notifyFramePresented();
        QVERIFY(!service.GetActive());
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed.last().first().toBool(), false);
    }

    void noDbusMethodCanUnlock()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        session::ScreenSaverService service(&controller);
        QVERIFY(service.registerService());
        QDBusPendingCallWatcher lock(call(QStringLiteral("Lock")));
        QTRY_VERIFY(lock.isFinished());
        QVERIFY(!lock.isError());
        QCOMPARE(controller.state(), session::SessionState::Locked);
        QDBusPendingCallWatcher unlock(call(QStringLiteral("Unlock")));
        QTRY_VERIFY(unlock.isFinished());
        QVERIFY(unlock.isError());
        QCOMPARE(controller.state(), session::SessionState::Locked);
    }

    void lockAndInhibitAnswerAtBothPaths()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        session::ScreenSaverService service(&controller);
        int count = 0;
        service.setInhibition([&count] { ++count; }, [&count] { --count; });
        QVERIFY(service.registerService());
        for (const QString path :
             {QStringLiteral("/ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver")}) {
            const QDBusMessage active = finished(call(QStringLiteral("GetActive"), {}, path));
            QCOMPARE(active.type(), QDBusMessage::ReplyMessage);
            const QDBusMessage cookie = finished(
                    call(QStringLiteral("Inhibit"), {QStringLiteral("test"), QStringLiteral("video")},
                         path));
            QCOMPARE(cookie.type(), QDBusMessage::ReplyMessage);
            QVERIFY(cookie.arguments().front().toUInt() != 0);
        }
        QCOMPARE(count, 2);
    }

    void inhibitCookiesBelongToTheirCaller()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        session::ScreenSaverService service(&controller);
        int count = 0;
        service.setInhibition([&count] { ++count; }, [&count] { --count; });
        QVERIFY(service.registerService());
        auto first = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                   QStringLiteral("screen-saver-first"));
        auto second = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                    QStringLiteral("screen-saver-second"));
        const QVariantList request {QStringLiteral("test"), QStringLiteral("video")};
        const QString path = QStringLiteral("/org/freedesktop/ScreenSaver");
        QDBusMessage reply = finished(call(QStringLiteral("Inhibit"), request, path, first));
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        const uint cookie = reply.arguments().front().toUInt();
        QVERIFY(cookie != 0);
        QCOMPARE(count, 1);
        // Another caller cannot release it.
        QCOMPARE(finished(call(QStringLiteral("UnInhibit"), {cookie}, path, second)).type(),
                 QDBusMessage::ReplyMessage);
        QCOMPARE(count, 1);
        reply = finished(call(QStringLiteral("Inhibit"), request, path, first));
        QVERIFY(reply.arguments().front().toUInt() != cookie);
        QCOMPARE(count, 2);
        finished(call(QStringLiteral("UnInhibit"), {cookie}, path, first));
        QCOMPARE(count, 1);
        finished(call(QStringLiteral("UnInhibit"), {cookie}, path, first));
        QCOMPARE(count, 1);
        // Leaving the bus releases everything the caller still holds. The
        // connection only closes once no QDBusConnection refers to it.
        QDBusConnection::disconnectFromBus(QStringLiteral("screen-saver-first"));
        first = QDBusConnection(QString());
        QTRY_COMPARE(count, 0);
        QDBusConnection::disconnectFromBus(QStringLiteral("screen-saver-second"));
    }

    void inhibitFromACallerThatAlreadyLeftIsReleased()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        session::ScreenSaverService service(&controller);
        int acquired = 0;
        int count = 0;
        service.setInhibition(
                [&] {
                    ++acquired;
                    ++count;
                },
                [&count] { --count; });
        QVERIFY(service.registerService());
        auto gone = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                  QStringLiteral("screen-saver-gone"));
        const QString name = gone.baseService();
        auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
                                                      QStringLiteral("/ScreenSaver"),
                                                      QStringLiteral("org.freedesktop.ScreenSaver"),
                                                      QStringLiteral("Inhibit"));
        message.setArguments({QStringLiteral("test"), QStringLiteral("video")});
        // Sent without waiting for the cookie, as dbus-send does. Once the
        // blocking call behind it is answered, the bus has routed the Inhibit.
        QVERIFY(gone.send(message));
        QVERIFY(gone.interface()->isServiceRegistered(name).value());
        QDBusConnection::disconnectFromBus(QStringLiteral("screen-saver-gone"));
        gone = QDBusConnection(QString());
        // Blocking calls process no events here, so the service handles the
        // Inhibit only after the bus has announced that the caller left.
        QDeadlineTimer deadline(5000);
        while (client.interface()->isServiceRegistered(name).value() && !deadline.hasExpired())
            QThread::msleep(10);
        QVERIFY(!client.interface()->isServiceRegistered(name).value());
        QTRY_COMPARE(acquired, 1);
        QTRY_COMPARE(count, 0);
    }

    void inhibitIsRefusedWithoutACounter()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure, nullptr, session::SessionState::Login);
        session::ScreenSaverService service(&controller);
        QVERIFY(service.registerService());
        const QDBusMessage reply = finished(
                call(QStringLiteral("Inhibit"), {QStringLiteral("test"), QStringLiteral("video")}));
        QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    }

    void loginRemainsDistinctAndBlocked()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure, nullptr, session::SessionState::Login);
        session::ScreenSaverService service(&controller);
        QVERIFY(service.registerService());
        QDBusPendingCallWatcher lock(call(QStringLiteral("Lock")));
        QTRY_VERIFY(lock.isFinished());
        QVERIFY(lock.isError());
        QVERIFY(service.GetActive());
        QCOMPARE(controller.state(), session::SessionState::Login);
    }
};
QTEST_GUILESS_MAIN(ScreenSaverServiceTest)
#include "screen_saver_service_test.moc"
