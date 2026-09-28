// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/auth_controller.h"
#include "auth/auth_ui.h"
#include "compositor/compositor.h"
#include "fake_backend.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/node.h"
#include "session/secure_container.h"
#include "session/session_controller.h"
#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <linux/input-event-codes.h>
#include <tuple>
using namespace cutefish;

class AuthUiTest : public QObject
{
    Q_OBJECT
private slots:

    void wallpaperIsBlurredPerOutputAndFailureKeepsShield()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString path = files.filePath(QStringLiteral("wallpaper.png"));
        QImage source(80, 40, QImage::Format_RGB32);
        source.fill(Qt::red);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 40; x < source.width(); ++x)
                source.setPixelColor(x, y, Qt::blue);
        QVERIFY(source.save(path));
        QSettings settings(QStringLiteral("cutefish"), QStringLiteral("theme"));
        settings.setValue(QStringLiteral("Wallpaper"), path);
        settings.sync();
        scene::Node root;
        session::SecureContainer secure(&root);
        secure.setOutputs({{QStringLiteral("one"), QRect(0, 0, 640, 320)},
                           {QStringLiteral("two"), QRect(640, 0, 320, 640)}});
        secure.setBlocked(true);
        testsupport::FakeRenderer renderer;
        const auto rendered = [&] {
            renderer.calls.clear();
            root.render(renderer);
            return renderer.calls.size();
        };
        QTRY_COMPARE(rendered(), 4);
        QCOMPARE(renderer.calls.at(0).kind, QStringLiteral("fill"));
        QCOMPARE(renderer.calls.at(0).color.a, 1.0f);
        const auto *texture = static_cast<testsupport::FakeTexture *>(renderer.calls.at(1).texture);
        QCOMPARE(texture->size(), QSize(80, 40));
        QImage image(texture->pixels().data(), 80, 40, QImage::Format_RGBA8888_Premultiplied);
        const QColor middle = image.pixelColor(40, 20);
        QVERIFY(middle.red() > 0 && middle.blue() > 0);
        QVERIFY(middle.red() + middle.blue() >= 245);
        QCOMPARE(image.pixelColor(0, 20), QColor(Qt::red));
        QCOMPARE(renderer.calls.at(3).texture->size(), QSize(40, 80));
        secure.setBlocked(false);
        settings.setValue(QStringLiteral("Wallpaper"), files.filePath(QStringLiteral("missing")));
        settings.sync();
        secure.setBlocked(true);
        QCOMPARE(rendered(), 2);
        for (const auto &call : renderer.calls) {
            QCOMPARE(call.kind, QStringLiteral("fill"));
            QCOMPARE(call.color.a, 1.0f);
            QCOMPARE(call.color.r, 0.0f);
        }
        settings.clear();
        settings.sync();
    }

    void prewarmedUiRepeatsKeysAndUpdatesCursor()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        scene::Node applications;
        compositor::Compositor compositor(&applications);
        input::Seat seat(&compositor);
        seat.setSessionGate(&controller);
        input::InputManager input(&seat);
        FakeBackend backend;
        auth::AuthUi ui(&controller, seat.xkbTracker(), &backend);
        ui.refresh();
        auto *window = ui.window();
        QVERIFY(window);
        QVERIFY(!window->handle());
        QVERIFY(!root.isVisible());
        QVERIFY(!ui.renderImage().isNull());
        QCOMPARE(backend.startCount, 0);
        QVERIFY(controller.requestLock());
        ui.refresh();
        QCOMPARE(ui.window(), window);
        QCOMPARE(backend.startCount, 1);
        auto *field = window->activeFocusItem();
        QVERIFY(field);
        field->setProperty("text", QStringLiteral("abcdefghij"));
        input.onKey({}, KEY_BACKSPACE, true);
        QCOMPARE(field->property("text").toString().size(), 9);
        QTRY_VERIFY_WITH_TIMEOUT(field->property("text").toString().size() < 9, 1000);
        input.onKey({}, KEY_BACKSPACE, false);
        const QString remaining = field->property("text").toString();
        QTest::qWait(120);
        QCOMPARE(field->property("text").toString(), remaining);
        QSignalSpy cursors(&ui, &auth::AuthUi::cursorShapeChanged);
        const QPointF fieldCenter =
                field->mapToScene(QPointF(field->width() / 2, field->height() / 2))
                + secure.authContainer()->children().front()->absolutePosition();
        ui.onPointerMotion({}, fieldCenter);
        QVERIFY(!cursors.isEmpty());
        QCOMPARE(cursors.last().at(0).value<Qt::CursorShape>(), Qt::IBeamCursor);
        ui.onPointerMotion({}, QPointF(0, 0));
        QCOMPARE(cursors.last().at(0).value<Qt::CursorShape>(), Qt::ArrowCursor);
        input.onKey({}, KEY_BACKSPACE, true);
        controller.notifyFramePresented();
        QVERIFY(controller.requestUnlock());
        const QString beforeUnlock = field->property("text").toString();
        QTest::qWait(600);
        QCOMPARE(field->property("text").toString(), beforeUnlock);
        input.onKey({}, KEY_BACKSPACE, false);
        seat.shutdown();
    }

    void wallpaperFollowsSettingsWhileUnlocked()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const auto save = [&](const QString &name, const QColor &color) {
            QImage image(80, 40, QImage::Format_RGB32);
            image.fill(color);
            const QString path = files.filePath(name);
            return image.save(path) ? path : QString();
        };
        QSettings settings(QStringLiteral("cutefish"), QStringLiteral("theme"));
        settings.setValue(QStringLiteral("Wallpaper"), save(QStringLiteral("red.png"), Qt::red));
        settings.sync();
        scene::Node root;
        session::SecureContainer secure(&root);
        secure.setOutputs({{QStringLiteral("one"), QRect(0, 0, 640, 320)}});
        testsupport::FakeRenderer renderer;
        const auto shown = [&] {
            renderer.calls.clear();
            secure.shieldContainer()->render(renderer);
            for (const auto &call : renderer.calls) {
                if (const auto *texture = static_cast<testsupport::FakeTexture *>(call.texture)) {
                    QImage image(texture->pixels().data(), texture->size().width(),
                                 texture->size().height(), QImage::Format_RGBA8888_Premultiplied);
                    return image.pixelColor(0, 0);
                }
            }
            return QColor();
        };
        QTRY_COMPARE(shown(), QColor(Qt::red));
        // Picked up from the settings file, with no lock to prompt a reread.
        settings.setValue(QStringLiteral("Wallpaper"), save(QStringLiteral("blue.png"), Qt::blue));
        settings.sync();
        QTRY_COMPARE(shown(), QColor(Qt::blue));
        // The same file, rewritten in place.
        save(QStringLiteral("blue.png"), Qt::green);
        QTRY_COMPARE(shown(), QColor(Qt::green));
        settings.clear();
        settings.sync();
    }

    void preparedPanelIdlesUntilTheNextLock()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        QVERIFY(ui.isRefreshPending());
        ui.refresh();
        QVERIFY(ui.window());
        QVERIFY(!ui.renderImage().isNull());
        // Nothing wakes the compositor for a hidden panel, not even the caret.
        QVERIFY(!ui.window()->activeFocusItem());
        QTest::qWait(100);
        QVERIFY(!ui.isRefreshPending());
        QVERIFY(controller.requestLock());
        QVERIFY(ui.isRefreshPending());
        // Locked, it waits for input or a scene change instead of polling.
        QTest::qWait(100);
        QVERIFY(!ui.isRefreshPending());
        QVERIFY(ui.window()->activeFocusItem());
        QVERIFY(ui.window()->activeFocusItem()->property("cursorVisible").toBool());
        ui.controller()->authenticate(QStringLiteral("valid"));
        backend.succeed();
        controller.notifyFramePresented();
        QTRY_COMPARE(controller.state(), session::SessionState::Unlocking);
        controller.notifyFramePresented();
        QCOMPARE(controller.state(), session::SessionState::Active);
        // Prepared again for the next lock, then idle. The preparation renders
        // the panel once more, which takes longer than 100 ms under ASan.
        QTRY_VERIFY_WITH_TIMEOUT(!ui.isRefreshPending(), 5000);
        QTest::qWait(100);
        QVERIFY(!ui.isRefreshPending());
        QVERIFY(ui.isPanelDrawn());
        QVERIFY(ui.window());
        QVERIFY(!ui.window()->activeFocusItem());
        QCOMPARE(ui.controller()->state(), auth::AuthController::Idle);
    }

    void panelFollowsOutputChanges()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        QTest::qWait(50);
        QVERIFY(!ui.isPanelDrawn());
        QVERIFY(!ui.isRefreshPending());
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        QTRY_VERIFY(ui.isPanelDrawn());
        const QSize size = ui.renderImage().size();
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600), 2}});
        QTRY_COMPARE(ui.renderImage().size(), size * 2);
    }

    void offscreenRenderingAndDirectInput()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        scene::Node applications;
        compositor::Compositor compositor(&applications);
        input::Seat seat(&compositor);
        seat.setSessionGate(&controller);
        input::InputManager input(&seat);
        FakeBackend backend;
        auth::AuthUi ui(&controller, seat.xkbTracker(), &backend);
        QVERIFY(controller.requestLock());
        controller.notifyFramePresented();
        ui.refresh();
        QVERIFY(ui.window());
        QVERIFY(!ui.window()->isVisible());
        QVERIFY(!ui.window()->handle());
        const QImage image = ui.renderImage();
        QVERIFY(!image.isNull());
        bool painted = false;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                painted |= qAlpha(image.pixel(x, y)) != 0;
        QVERIFY(painted);
        input.onKey({}, KEY_A, true);
        input.onKey({}, KEY_A, false);
        input.onKey({}, KEY_ENTER, true);
        input.onKey({}, KEY_ENTER, false);
        QCOMPARE(backend.authenticateCount, 1);
        QCOMPARE(backend.lastSecret, QStringLiteral("a"));
        backend.reject();
        QCOMPARE(controller.state(), session::SessionState::Locked);
        backend.succeed();
        QCOMPARE(controller.state(), session::SessionState::Locked);
        ui.controller()->authenticate(QStringLiteral("valid"));
        backend.succeed();
        backend.succeed();
        QCOMPARE(backend.acceptCount, 1);
        QCOMPARE(controller.state(), session::SessionState::Unlocking);
        QVERIFY(root.isVisible());
        controller.notifyFramePresented();
        QCOMPARE(controller.state(), session::SessionState::Active);
        QVERIFY(controller.requestLock());
        controller.notifyFramePresented();
        ui.refresh();
        QCOMPARE(ui.controller()->state(), auth::AuthController::Idle);
        seat.shutdown();
    }

    void submittedPasswordStaysUntilRejected()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        scene::Node applications;
        compositor::Compositor compositor(&applications);
        input::Seat seat(&compositor);
        seat.setSessionGate(&controller);
        input::InputManager input(&seat);
        FakeBackend backend;
        auth::AuthUi ui(&controller, seat.xkbTracker(), &backend);
        QVERIFY(controller.requestLock());
        controller.notifyFramePresented();
        ui.refresh();
        auto *field = ui.window()->activeFocusItem();
        QVERIFY(field);
        const auto submit = [&](int key) {
            input.onKey({}, key, true);
            input.onKey({}, key, false);
            input.onKey({}, KEY_ENTER, true);
            input.onKey({}, KEY_ENTER, false);
        };
        submit(KEY_A);
        QCOMPARE(backend.authenticateCount, 1);
        QCOMPARE(field->property("text").toString(), QStringLiteral("a"));
        QVERIFY(!field->isEnabled());
        backend.reject();
        QCOMPARE(field->property("text").toString(), QString());
        QVERIFY(field->isEnabled());
        QVERIFY(field->hasActiveFocus());
        submit(KEY_B);
        QCOMPARE(backend.lastSecret, QStringLiteral("b"));
        backend.succeed();
        QCOMPARE(controller.state(), session::SessionState::Unlocking);
        QCOMPARE(field->property("text").toString(), QStringLiteral("b"));
        QVERIFY(!field->isEnabled());
        seat.shutdown();
    }

    void panelIsDrawnAtTheOutputDensity()
    {
        const auto paintedArea = [](double scale) {
            scene::Node root;
            session::SecureContainer secure(&root);
            session::SessionController controller(&secure);
            controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600), scale}});
            FakeBackend backend;
            auth::AuthUi ui(&controller, nullptr, &backend);
            ui.refresh();
            const QImage image = ui.renderImage();
            QRect painted;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) != 0)
                        painted |= QRect(x, y, 1, 1);
            return std::tuple(image.size(), image.devicePixelRatio(), painted);
        };
        const auto [plainSize, plainRatio, plain] = paintedArea(1);
        const auto [denseSize, denseRatio, dense] = paintedArea(1.5);
        QCOMPARE(plainSize, QSize(480, 560));
        QCOMPARE(plainRatio, 1.0);
        // One texel per device pixel, and the content itself drawn larger, not
        // a logical-size drawing in the corner of a larger image.
        QCOMPARE(denseSize, QSize(720, 840));
        QCOMPARE(denseRatio, 1.5);
        QVERIFY(!plain.isEmpty());
        QVERIFY(std::abs(dense.width() - plain.width() * 1.5) <= 2);
        QVERIFY(std::abs(dense.height() - plain.height() * 1.5) <= 2);
        QVERIFY(std::abs(dense.left() - plain.left() * 1.5) <= 2);
    }

    void choiceMenuClosesOnAPressElsewhere()
    {
        QTemporaryDir files;
        QFile qml(files.filePath(QStringLiteral("menu.qml")));
        QVERIFY(qml.open(QIODevice::WriteOnly));
        qml.write("import QtQuick\n"
                  "import \"qrc:/auth\"\n"
                  "Item {\n"
                  "    function focusPassword() {}\n"
                  "    property alias menuOpen: menu.open\n"
                  "    ChoiceMenu {\n"
                  "        id: menu\n"
                  "        x: 100; y: 300; width: 160\n"
                  "        names: [\"Cutefish\", \"Plasma\"]\n"
                  "        currentIndex: 0\n"
                  "    }\n"
                  "}\n");
        qml.close();
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend, QUrl::fromLocalFile(qml.fileName()));
        QVERIFY(controller.requestLock());
        controller.notifyFramePresented();
        ui.refresh();
        QVERIFY(ui.window());
        // The menu's overlay sits next to it under the content item.
        QQuickItem *screen = nullptr;
        for (auto *item : ui.window()->contentItem()->childItems()) {
            if (item->property("menuOpen").isValid())
                screen = item;
        }
        QVERIFY(screen);
        const QPointF panel = secure.authContainer()->children().front()->absolutePosition();
        const auto click = [&](QPointF point) {
            ui.onPointerMotion({}, panel + point);
            ui.onPointerButton({}, panel + point, BTN_LEFT, true);
            ui.onPointerButton({}, panel + point, BTN_LEFT, false);
        };
        click({180, 315});
        QVERIFY(screen->property("menuOpen").toBool());
        // Elsewhere closes it; on the button it toggles as before.
        click({20, 20});
        QVERIFY(!screen->property("menuOpen").toBool());
        click({180, 315});
        QVERIFY(screen->property("menuOpen").toBool());
        click({180, 315});
        QVERIFY(!screen->property("menuOpen").toBool());
    }

    void roundedRectangleIsOnePathOnDevicePixels()
    {
        // Half a device pixel off the grid at 1.5, where the software renderer's
        // own rounded Rectangle seams through the middle and steps at the corners.
        QTemporaryDir files;
        QFile qml(files.filePath(QStringLiteral("pill.qml")));
        QVERIFY(qml.open(QIODevice::WriteOnly));
        qml.write("import QtQuick\n"
                  "import Cutefish.Auth\n"
                  "Item {\n"
                  "    function focusPassword() {}\n"
                  "    RoundedRectangle {\n"
                  "        x: 20; y: 20.5; width: 120; height: 38; radius: 19\n"
                  "        color: Qt.rgba(1, 1, 1, 0.25)\n"
                  "        border.width: 1\n"
                  "        border.color: Qt.rgba(1, 1, 1, 0.5)\n"
                  "    }\n"
                  "}\n");
        qml.close();
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600), 1.5}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend, QUrl::fromLocalFile(qml.fileName()));
        ui.refresh();
        const QImage image = ui.renderImage();
        QVERIFY(!image.isNull());
        const int centre = 120;
        int top = -1, bottom = -1;
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(centre, y)) != 0) {
                bottom = y;
                if (top < 0)
                    top = y;
            }
        }
        QVERIFY(top > 0 && bottom > top + 4);
        // Whole device pixels on both sides: the border is as heavy at the bottom as at the top.
        QCOMPARE(image.pixel(centre, bottom), image.pixel(centre, top));
        QVERIFY(image.pixel(centre, top) != image.pixel(centre, top + 1));
        // The fill is one colour everywhere inside, the rounded ends included.
        const QRgb fill = image.pixel(centre, (top + bottom) / 2);
        for (int x = 32; x < 208; ++x)
            QCOMPARE(image.pixel(x, (top + bottom) / 2), fill);
        for (int y = top + 1; y < bottom; ++y)
            QCOMPARE(image.pixel(centre, y), fill);
    }

    void outputChangesReuseOneOffscreenWindow()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        controller.requestLock();
        ui.refresh();
        auto *window = ui.window();
        QVERIFY(window);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)},
                               {QStringLiteral("two"), QRect(800, 0, 640, 480)}});
        ui.refresh();
        QCOMPARE(ui.window(), window);
        QCOMPARE(secure.authContainer()->children().size(), size_t(1));
        controller.setOutputs({{QStringLiteral("two"), QRect(800, 0, 640, 480)}});
        ui.refresh();
        QCOMPARE(ui.window(), window);
        QVERIFY(QRect(800, 0, 640, 480)
                        .contains(secure.authContainer()->children().front()->geometry()));
        controller.setOutputs({});
        ui.refresh();
        QVERIFY(ui.renderImage().isNull());
        controller.setOutputs({{QStringLiteral("two"), QRect(0, 0, 640, 480)}});
        ui.refresh();
        QVERIFY(!ui.renderImage().isNull());
        QCOMPARE(ui.window(), window);
        QVERIFY(!window->handle());
    }

    void panelTextureFollowsTheImageSize()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        controller.requestLock();
        ui.refresh();
        secure.finishTransition();
        testsupport::FakeRenderer renderer;
        // The panel is drawn last, above the shield and its wallpaper.
        const auto panelTexture = [&]() -> const render::Texture * {
            renderer.calls.clear();
            root.render(renderer);
            for (auto call = renderer.calls.crbegin(); call != renderer.calls.crend(); ++call) {
                if (call->texture)
                    return call->texture;
            }
            return nullptr;
        };
        const render::Texture *texture = panelTexture();
        QVERIFY(texture);
        QCOMPARE(texture->size(), QSize(480, 560));
        // A denser output redraws the panel larger. The renderer takes the
        // texture's own size for an update, so the texture has to be replaced.
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600), 2}});
        ui.refresh();
        texture = panelTexture();
        QVERIFY(texture);
        QCOMPARE(ui.renderImage().size(), QSize(960, 1120));
        QCOMPARE(texture->size(), QSize(960, 1120));
    }

    void textureFailureKeepsCompositorShield()
    {
        class FailedRenderer : public testsupport::FakeRenderer
        {
        public:
            std::unique_ptr<render::Texture> createTexture(const QSize &, const uint8_t *, int,
                                                           render::PixelFormat) override
            {
                return {};
            }
        } renderer;

        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        controller.requestLock();
        ui.refresh();
        // Once the lock has faded in, the shield alone covers the output.
        secure.finishTransition();
        root.render(renderer);
        QCOMPARE(renderer.calls.size(), 1);
        QCOMPARE(renderer.calls.front().kind, QStringLiteral("fill"));
        QCOMPARE(renderer.calls.front().color.a, 1.0f);
        QCOMPARE(renderer.calls.front().color.r, 0.0f);
        QVERIFY(controller.isBlocked());
    }

    void successDuringLockingWaitsForSecurePresentation()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        controller.requestLock();
        ui.refresh();
        ui.controller()->authenticate(QStringLiteral("valid"));
        backend.succeed();
        QCOMPARE(controller.state(), session::SessionState::Locking);
        controller.notifyFramePresented();
        ui.refresh();
        QCOMPARE(controller.state(), session::SessionState::Unlocking);
        backend.succeed();
        QCOMPARE(backend.acceptCount, 1);
        controller.notifyFramePresented();
        QCOMPARE(controller.state(), session::SessionState::Active);
    }

    void failedQmlLeavesShieldOnEveryOutput()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend, QUrl(QStringLiteral("qrc:/missing.qml")));
        controller.requestLock();
        ui.refresh();
        QVERIFY(!ui.window());
        QVERIFY(root.isVisible());
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)},
                               {QStringLiteral("two"), QRect(800, 0, 640, 480)}});
        ui.refresh();
        QCOMPARE(secure.shieldContainer()->children().size(), size_t(2));
        controller.notifyOutputPresented(QStringLiteral("one"));
        QCOMPARE(controller.state(), session::SessionState::Locking);
        controller.notifyOutputPresented(QStringLiteral("two"));
        QCOMPARE(controller.state(), session::SessionState::Locked);
        controller.setOutputs({});
        ui.refresh();
        QVERIFY(controller.isBlocked());
        controller.setOutputs({{QStringLiteral("two"), QRect(0, 0, 640, 480)}});
        ui.refresh();
        QVERIFY(root.isVisible());
        QCOMPARE(secure.shieldContainer()->children().size(), size_t(1));
    }

    void failedQmlStillTakesThePassword()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        scene::Node applications;
        compositor::Compositor compositor(&applications);
        input::Seat seat(&compositor);
        seat.setSessionGate(&controller);
        input::InputManager input(&seat);
        FakeBackend backend;
        auth::AuthUi ui(&controller, seat.xkbTracker(), &backend,
                        QUrl(QStringLiteral("qrc:/missing.qml")));
        QVERIFY(controller.requestLock());
        ui.refresh();
        QVERIFY(!ui.window());
        const auto drawn = [&] {
            const QImage image = ui.renderImage();
            int opaque = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    opaque += qAlpha(image.pixel(x, y)) > 0;
            return opaque;
        };
        QVERIFY(drawn() > 0);
        const auto type = [&](std::initializer_list<uint32_t> keys) {
            for (uint32_t key : keys) {
                input.onKey({}, key, true);
                input.onKey({}, key, false);
            }
        };
        const quint64 empty = ui.imageGeneration();
        type({KEY_P, KEY_W, KEY_D, KEY_X, KEY_BACKSPACE});
        QVERIFY(drawn() > 0);
        QVERIFY(ui.imageGeneration() > empty);
        type({KEY_ENTER});
        QCOMPARE(backend.authenticateCount, 1);
        QCOMPARE(backend.lastSecret, QStringLiteral("pwd"));
        backend.reject();
        QVERIFY(ui.controller()->failed());
        // A rejection clears the field, so the next attempt starts empty.
        type({KEY_O, KEY_K, KEY_ENTER});
        QCOMPARE(backend.authenticateCount, 2);
        QCOMPARE(backend.lastSecret, QStringLiteral("ok"));
        backend.succeed();
        controller.notifyFramePresented();
        QTRY_COMPARE(controller.state(), session::SessionState::Unlocking);
        seat.shutdown();
    }

    void panelOpensOnThePointersScreen()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        const QRect one(0, 0, 800, 600);
        const QRect two(800, 0, 1000, 700);
        controller.setOutputs({{QStringLiteral("one"), one}, {QStringLiteral("two"), two, 1, true}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        QPointF pointer(-10, -10);
        ui.setPointerLocator([&] {
            return pointer;
        });
        QVERIFY(controller.requestLock());
        QTRY_VERIFY(ui.isPanelDrawn());
        const scene::Node *panel = secure.authContainer()->children().front();
        // With the pointer on no screen, the panel takes the primary one.
        QVERIFY(two.contains(panel->absoluteGeometry()));
        pointer = QPointF(100, 100);
        ui.onPointerMotion({}, pointer);
        QVERIFY(ui.isRefreshPending());
        QTRY_VERIFY(one.contains(panel->absoluteGeometry()));
        // Moving within that screen leaves the panel where it is.
        const QRect placed = panel->absoluteGeometry();
        pointer = QPointF(300, 300);
        ui.onPointerMotion({}, pointer);
        QTest::qWait(50);
        QCOMPARE(panel->absoluteGeometry(), placed);
    }

    void loginUsesSameOffscreenScreenAndStaysBlockedForHandoff()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure, nullptr, session::SessionState::Login);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        FakeBackend backend;
        auth::AuthUi ui(&controller, nullptr, &backend);
        ui.refresh();
        QVERIFY(!ui.renderImage().isNull());
        QVERIFY(!ui.window()->handle());
        QCOMPARE(ui.controller()->mode(), auth::AuthController::Login);
        ui.controller()->authenticate(QStringLiteral("valid"));
        backend.succeed();
        QCOMPARE(backend.acceptCount, 1);
        QCOMPARE(controller.state(), session::SessionState::Login);
        QVERIFY(root.isVisible());
    }
};

int main(int argc, char **argv)
{
    auth::AuthUi::configurePlatform();
    QGuiApplication app(argc, argv);
    QTemporaryDir configuration;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, configuration.path());
    AuthUiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "auth_ui_test.moc"
