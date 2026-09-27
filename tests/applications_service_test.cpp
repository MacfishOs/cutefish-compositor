// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "fake_renderer.h"
#include "input/seat.h"
#include "scene/scene.h"
#include "session/applications_service.h"
#include "shm_test_util.h"
#include "wm/quit_all.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTest>

using namespace cutefish;

class ApplicationsServiceTest : public QObject
{
    Q_OBJECT
private:
    QDBusConnection client = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("applications-test-client"));

    struct Desktop {
        scene::Scene scene;
        scene::Node *surfaces =
                scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
        compositor::Compositor compositor {surfaces};
        testsupport::FakeRenderer renderer;
        input::Seat seat {&compositor};
        wm::WindowManager manager {&compositor, &seat};
        std::vector<std::unique_ptr<compositor::Surface>> owned;
        std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 20);

        ~Desktop() { seat.shutdown(); }

        wm::Window *open(const QString &applicationId)
        {
            owned.push_back(compositor.createSurface());
            auto *surface = owned.back().get();
            auto buffer = pool->createBuffer({200, 100}, 0, 800, render::PixelFormat::Bgrx8888,
                                             nullptr);
            surface->attach(buffer, {});
            surface->commit();
            compositor.updateTextures(renderer);
            return manager.createWindow(surface, {.requestedSize = {200, 100},
                                                  .applicationId = applicationId,
                                                  .closeCallback = [] {}});
        }
    };

    QDBusPendingCall quitAll()
    {
        return client.asyncCall(QDBusMessage::createMethodCall(
                QStringLiteral("com.cutefish.Applications1"),
                QStringLiteral("/com/cutefish/Applications1"),
                QStringLiteral("com.cutefish.Applications1"), QStringLiteral("QuitAll")));
    }

private slots:
    void initTestCase() { QVERIFY(client.isConnected()); }

    void repliesOnceEveryApplicationHasQuit()
    {
        Desktop desktop;
        wm::Window *editor = desktop.open(QStringLiteral("kate"));
        wm::QuitAll quit(&desktop.manager);
        std::vector<bool> ending;
        session::ApplicationsService service(&quit, [&ending](bool value) {
            ending.push_back(value);
        });
        QVERIFY(service.registerService());

        QDBusPendingCallWatcher first(quitAll());
        QDBusPendingCallWatcher second(quitAll());
        QTRY_VERIFY(quit.isRunning());
        QTest::qWait(50);
        QVERIFY(!first.isFinished());
        QCOMPARE(ending, std::vector<bool> {true});

        desktop.manager.destroyWindow(editor);
        QTRY_VERIFY(first.isFinished() && second.isFinished());
        for (auto *watcher : {&first, &second}) {
            const QDBusPendingReply<QString, QString> reply = *watcher;
            QVERIFY(!reply.isError());
            QCOMPARE(reply.argumentAt<0>(), QStringLiteral("quit"));
        }
        // The session ends from here, with the docks still away.
        QCOMPARE(ending, std::vector<bool> {true});
    }

    void aCancelledQuitBringsTheDocksBack()
    {
        Desktop desktop;
        desktop.open(QStringLiteral("frozen"));
        qint64 now = 0;
        wm::QuitAll quit(&desktop.manager, [&now] {
            return now;
        });
        std::vector<bool> ending;
        session::ApplicationsService service(&quit, [&ending](bool value) {
            ending.push_back(value);
        });
        QVERIFY(service.registerService());

        QDBusPendingCallWatcher call(quitAll());
        QTRY_VERIFY(quit.isRunning());
        now = wm::QuitAll::kUnresponsiveAfterMs + 1;
        QTRY_VERIFY(call.isFinished());
        const QDBusPendingReply<QString, QString> reply = call;
        QCOMPARE(reply.argumentAt<0>(), QStringLiteral("unresponsive"));
        QCOMPARE(reply.argumentAt<1>(), QStringLiteral("frozen"));
        QCOMPARE(ending, (std::vector<bool> {true, false}));
    }
};

QTEST_GUILESS_MAIN(ApplicationsServiceTest)

#include "applications_service_test.moc"
