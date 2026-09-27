// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/auth_controller.h"
#include "auth/greetd_backend.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace cutefish::auth;

class GreetdBackendTest : public QObject
{
    Q_OBJECT
private slots:

    void cancellationAcknowledgementCannotAuthenticate()
    {
        QTemporaryDir directory;
        QLocalServer server;
        const QString path = directory.filePath(QStringLiteral("greetd"));
        QVERIFY(server.listen(path));
        qputenv("GREETD_SOCK", path.toUtf8());
        GreetdBackend backend;
        AuthController controller(AuthController::Login, &backend);
        QSignalSpy success(&controller, &AuthController::authenticationSucceeded);
        controller.setUserName(QStringLiteral("test-user"));
        controller.setSessions({{QStringLiteral("gnome"), QStringLiteral("GNOME"),
                                 {QStringLiteral("gnome-session")},
                                 {QStringLiteral("XDG_SESSION_TYPE=wayland")}}});
        backend.start();
        QTRY_VERIFY(server.hasPendingConnections());
        std::unique_ptr<QLocalSocket> peer(server.nextPendingConnection());
        const auto send = [&](const QJsonObject &message) {
            const QByteArray data = QJsonDocument(message).toJson(QJsonDocument::Compact);
            const quint32 length = data.size();
            peer->write(reinterpret_cast<const char *>(&length), 4);
            peer->write(data);
            peer->flush();
        };
        const auto request = [&]() -> QJsonObject {
            QByteArray data;
            for (int i = 0; i < 100; ++i) {
                QCoreApplication::processEvents();
                data += peer->readAll();
                if (data.size() >= 4) {
                    quint32 length;
                    memcpy(&length, data.constData(), 4);
                    if (data.size() == 4 + length)
                        return QJsonDocument::fromJson(data.mid(4)).object();
                }
                QTest::qWait(1);
            }
            return {};
        };
        controller.authenticate(QStringLiteral("wrong"));
        QCOMPARE(request()["type"].toString(), QStringLiteral("create_session"));
        send({{"type", "error"}, {"error_type", "auth_error"}});
        QTRY_VERIFY(controller.failed());
        controller.reset();
        QCOMPARE(request()["type"].toString(), QStringLiteral("cancel_session"));
        controller.authenticate(QStringLiteral("right"));
        send({{"type", "success"}});
        QCOMPARE(request()["type"].toString(), QStringLiteral("create_session"));
        QCOMPARE(success.count(), 0);
        send({{"type", "auth_message"}, {"auth_message_type", "secret"}});
        QCOMPARE(request()["type"].toString(), QStringLiteral("post_auth_message_response"));
        send({{"type", "success"}});
        const QJsonObject start = request();
        QCOMPARE(start["type"].toString(), QStringLiteral("start_session"));
        // cutefish-dm starts the session by id; greetd runs the command.
        QCOMPARE(start["session"].toString(), QStringLiteral("gnome"));
        QCOMPARE(start["cmd"].toArray().first().toString(), QStringLiteral("gnome-session"));
        QVERIFY(start["env"].toArray().contains(QStringLiteral("XDG_SESSION_TYPE=wayland")));
        QCOMPARE(success.count(), 0);
        send({{"type", "error"}});
        QTRY_VERIFY(controller.failed());
        QCOMPARE(success.count(), 0);
        qunsetenv("GREETD_SOCK");
    }
};
QTEST_GUILESS_MAIN(GreetdBackendTest)
#include "greetd_backend_test.moc"
