// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/scene.h"
#include "session/window_debug_service.h"
#include "shm_test_util.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include <QDBusConnection>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace cutefish;

class WindowDebugServiceTest : public QObject
{
    Q_OBJECT
private:
    QDBusConnection client = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("window-debug-test-client"));

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

        Desktop() { manager.setOutputBounds(QRect(0, 0, 1280, 800)); }
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

    QDBusMessage call(const QString &method, const QVariantList &arguments = {})
    {
        QDBusMessage message = QDBusMessage::createMethodCall(
                QStringLiteral("com.cutefish.WindowDebug1"),
                QStringLiteral("/com/cutefish/WindowDebug1"),
                QStringLiteral("com.cutefish.WindowDebug1"), method);
        message.setArguments(arguments);
        // The service answers on this thread, so the wait has to keep its event loop running.
        return client.call(message, QDBus::BlockWithGui);
    }

    QJsonArray listWindows()
    {
        const QDBusReply<QString> reply = call(QStringLiteral("ListWindows"));
        return QJsonDocument::fromJson(reply.value().toUtf8()).array();
    }

private slots:
    void initTestCase() { QVERIFY(client.isConnected()); }

    void listsTheWindowsBottomToTop()
    {
        Desktop desktop;
        wm::Window *editor = desktop.open(QStringLiteral("kate"));
        wm::Window *viewer = desktop.open(QStringLiteral("loupe"));
        editor->setTitle(QStringLiteral("notes.txt"));
        session::WindowDebugService service(&desktop.manager);
        QVERIFY(service.registerService());

        const QJsonArray windows = listWindows();
        QCOMPARE(windows.size(), 2);
        const QJsonObject bottom = windows.at(0).toObject();
        QCOMPARE(bottom.value(QStringLiteral("id")).toString(), QString::number(editor->id()));
        QCOMPARE(bottom.value(QStringLiteral("appId")).toString(), QStringLiteral("kate"));
        QCOMPARE(bottom.value(QStringLiteral("title")).toString(), QStringLiteral("notes.txt"));
        QCOMPARE(bottom.value(QStringLiteral("state")).toString(), QStringLiteral("normal"));
        const QJsonArray bounds = bottom.value(QStringLiteral("bounds")).toArray();
        QCOMPARE(QRect(bounds.at(0).toInt(), bounds.at(1).toInt(), bounds.at(2).toInt(),
                       bounds.at(3).toInt()),
                 editor->bounds());
        QCOMPARE(windows.at(1).toObject().value(QStringLiteral("appId")).toString(),
                 QStringLiteral("loupe"));
        QCOMPARE(windows.at(1).toObject().value(QStringLiteral("id")).toString(),
                 QString::number(viewer->id()));
    }

    void movesResizesAndActivatesAWindow()
    {
        Desktop desktop;
        wm::Window *editor = desktop.open(QStringLiteral("kate"));
        wm::Window *viewer = desktop.open(QStringLiteral("loupe"));
        viewer->activate();
        session::WindowDebugService service(&desktop.manager);
        QVERIFY(service.registerService());
        const qulonglong id = editor->id();

        QVERIFY(QDBusReply<bool>(call(QStringLiteral("MoveWindow"), {id, 40, 60})).value());
        QCOMPARE(editor->bounds().topLeft(), QPoint(40, 60));
        QCOMPARE(editor->bounds().size(), QSize(200, 100));

        QVERIFY(QDBusReply<bool>(
                        call(QStringLiteral("SetWindowBounds"), {id, 100, 120, 300, 200}))
                        .value());
        QCOMPARE(editor->bounds(), QRect(100, 120, 300, 200));

        QVERIFY(QDBusReply<bool>(call(QStringLiteral("ActivateWindow"), {id})).value());
        QVERIFY(editor->isActive());
        QCOMPARE(desktop.manager.windows().back(), editor);

        QVERIFY(QDBusReply<bool>(call(QStringLiteral("SnapWindow"), {id, false})).value());
        QCOMPARE(editor->stateType(), wm::WindowStateType::SecondarySnapped);
        QVERIFY(QDBusReply<bool>(call(QStringLiteral("RestoreWindow"), {id})).value());
        QCOMPARE(editor->stateType(), wm::WindowStateType::Normal);

        QVERIFY(QDBusReply<bool>(call(QStringLiteral("MinimizeWindow"), {id})).value());
        QCOMPARE(editor->stateType(), wm::WindowStateType::Minimized);
    }

    void feedsInputOnlyWhenGivenOne()
    {
        Desktop desktop;
        session::WindowDebugService service(&desktop.manager);
        QVERIFY(service.registerService());
        QVERIFY(!QDBusReply<bool>(call(QStringLiteral("PointerMotion"), {40.0, 30.0})).value());

        input::InputManager input(&desktop.seat);
        input.setBounds({1280, 800});
        service.setInput(&input);
        QVERIFY(QDBusReply<bool>(call(QStringLiteral("PointerMotion"), {40.0, 30.0})).value());
        QCOMPARE(desktop.seat.pointerLocation(), QPointF(40, 30));
        QVERIFY(QDBusReply<bool>(call(QStringLiteral("PointerButton"), {uint(0x110), true})).value());
        QVERIFY(desktop.seat.pressedButtons() != input::NoButton);
        QVERIFY(QDBusReply<bool>(call(QStringLiteral("PointerButton"), {uint(0x110), false})).value());
        QCOMPARE(desktop.seat.pressedButtons(), input::NoButton);
    }

    void anUnknownIdChangesNothing()
    {
        Desktop desktop;
        wm::Window *editor = desktop.open(QStringLiteral("kate"));
        const QRect before = editor->bounds();
        session::WindowDebugService service(&desktop.manager);
        QVERIFY(service.registerService());

        const qulonglong missing = editor->id() + 1000;
        QVERIFY(!QDBusReply<bool>(call(QStringLiteral("MoveWindow"), {missing, 0, 0})).value());
        QVERIFY(!QDBusReply<bool>(call(QStringLiteral("ActivateWindow"), {missing})).value());
        QCOMPARE(editor->bounds(), before);
    }
};

QTEST_GUILESS_MAIN(WindowDebugServiceTest)
#include "window_debug_service_test.moc"
