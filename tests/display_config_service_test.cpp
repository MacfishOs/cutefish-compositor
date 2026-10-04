// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <limits>
#include <optional>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTest>

#include "display/display_configuration_controller.h"
#include "display/display_manager.h"
#include "fake_native_display_delegate.h"
#include "ipc/display_config_service.h"

using namespace cutefish;

namespace {
const QString kService = QStringLiteral("com.cutefish.DisplayConfig.ServiceTest");
} // namespace

class DisplayConfigServiceTest : public QObject
{
    Q_OBJECT

private:
    struct NightLightCall {
        double strength;
        std::chrono::milliseconds duration;
    };

    // The session is never on screen here: the night light goes through anyway.
    struct Service {
        display::DisplayManager manager;
        test::FakeNativeDisplayDelegate delegate;
        display::DisplayConfigurationController controller {&manager, &delegate};
        display::ipc::DisplayConfigService service {&manager, &controller,
                                                    QDBusConnection::sessionBus(),
                                                    [] { return false; }};
        std::optional<NightLightCall> nightLight;

        Service()
        {
            service.setNightLightHandler([this](double strength, std::chrono::milliseconds duration) {
                nightLight = NightLightCall {strength, duration};
            });
        }

        ~Service() { QDBusConnection::sessionBus().unregisterService(kService); }
    };

    QDBusConnection client = QDBusConnection::connectToBus(
            QDBusConnection::SessionBus, QStringLiteral("display-config-service-test-client"));

    // Waits with the event loop running: the service answers from this thread.
    QDBusMessage call(const QString &method, const QVariantList &arguments)
    {
        auto message = QDBusMessage::createMethodCall(kService,
                                                      QStringLiteral("/com/cutefish/DisplayConfig1"),
                                                      QStringLiteral("com.cutefish.DisplayConfig1"),
                                                      method);
        message.setArguments(arguments);
        QDBusPendingCallWatcher watcher(client.asyncCall(message));
        if (!QTest::qWaitFor([&] { return watcher.isFinished(); }))
            return {};
        return watcher.reply();
    }

private slots:
    void initTestCase() { QVERIFY(client.isConnected()); }

    void nightLightReachesTheCompositorWhileTheSessionIsInactive()
    {
        Service service;
        QVERIFY(service.service.registerService(kService));

        const auto reply = call(QStringLiteral("SetNightLight"), {0.5, 1000u});
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QVERIFY(service.nightLight);
        QCOMPARE(service.nightLight->strength, 0.5);
        QCOMPARE(service.nightLight->duration.count(), 1000);
    }

    void theStrengthIsClampedToTheRange()
    {
        Service service;
        QVERIFY(service.service.registerService(kService));

        call(QStringLiteral("SetNightLight"), {2.5, 0u});
        QVERIFY(service.nightLight);
        QCOMPARE(service.nightLight->strength, 1.0);
        call(QStringLiteral("SetNightLight"), {-1.0, 0u});
        QCOMPARE(service.nightLight->strength, 0.0);
    }

    void aStrengthThatIsNotANumberIsRefused()
    {
        Service service;
        QVERIFY(service.service.registerService(kService));

        const auto reply = call(QStringLiteral("SetNightLight"),
                                {std::numeric_limits<double>::quiet_NaN(), 0u});
        QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));
        QVERIFY(!service.nightLight);
    }

    // The night light's exception does not open the configuration writes.
    void otherWritesStillNeedTheSessionOnScreen()
    {
        Service service;
        QVERIFY(service.service.registerService(kService));

        const auto reply = call(QStringLiteral("RevertConfiguration"), {QString()});
        QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
        QCOMPARE(reply.errorName(),
                 QStringLiteral("com.cutefish.DisplayConfig1.Error.Unauthorized"));
    }
};

QTEST_GUILESS_MAIN(DisplayConfigServiceTest)
#include "display_config_service_test.moc"
