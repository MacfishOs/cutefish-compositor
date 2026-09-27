// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/touchscreen_settings.h"

#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>

using cutefish::input::TouchscreenSettings;

namespace {
const QString elan = QStringLiteral("ELAN2514:00 04F3:2AF1");
const QString wacom = QStringLiteral("Wacom HID 5256 Finger");
} // namespace

TEST(TouchscreenSettings, EveryDeviceStartsEnabled)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TouchscreenSettings settings(nullptr, directory.filePath(QStringLiteral("touchscreen.conf")));
    EXPECT_TRUE(settings.devices().isEmpty());
    EXPECT_TRUE(settings.disabledDevices().isEmpty());
    EXPECT_TRUE(settings.deviceEnabled(elan));
}

TEST(TouchscreenSettings, PersistsPerDeviceAndNotifiesOnlyOnChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("touchscreen.conf"));
    {
        TouchscreenSettings settings(nullptr, file);
        QSignalSpy changes(&settings, &TouchscreenSettings::settingsChanged);
        QSignalSpy disabled(&settings, &TouchscreenSettings::disabledDevicesChanged);
        settings.setDeviceEnabled(elan, false);
        settings.setDeviceEnabled(elan, false);
        settings.setDeviceEnabled(wacom, true);
        settings.setDeviceEnabled(QString(), false);
        EXPECT_EQ(changes.count(), 1);
        EXPECT_EQ(disabled.count(), 1);
        EXPECT_FALSE(settings.deviceEnabled(elan));
        EXPECT_TRUE(settings.deviceEnabled(wacom));

        QSignalSpy devices(&settings, &TouchscreenSettings::devicesChanged);
        settings.setDevices({elan});
        settings.setDevices({elan});
        settings.setDevices({});
        EXPECT_EQ(devices.count(), 2);
        EXPECT_EQ(changes.count(), 1);
    }
    {
        TouchscreenSettings restored(nullptr, file);
        EXPECT_EQ(restored.disabledDevices(), QStringList{elan});
        EXPECT_TRUE(restored.devices().isEmpty());
        restored.setDeviceEnabled(elan, true);
    }
    TouchscreenSettings restored(nullptr, file);
    EXPECT_TRUE(restored.disabledDevices().isEmpty());
}

TEST(TouchscreenSettings, DBusPropertiesMethodsAndServiceRestart)
{
    ASSERT_TRUE(QDBusConnection::sessionBus().isConnected());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("touchscreen.conf"));
    for (int iteration = 0; iteration < 2; ++iteration) {
        TouchscreenSettings settings(nullptr, file);
        ASSERT_TRUE(settings.registerService());
        QDBusInterface proxy(QStringLiteral("com.cutefish.Touchscreen1"),
                             QStringLiteral("/com/cutefish/Touchscreen1"),
                             QStringLiteral("com.cutefish.Touchscreen1"));
        ASSERT_TRUE(proxy.isValid());
        EXPECT_EQ(proxy.property("disabledDevices").toStringList().isEmpty(), iteration == 0);
        settings.setDevices({elan, wacom});
        EXPECT_EQ(proxy.property("devices").toStringList(), (QStringList{elan, wacom}));

        QDBusPendingCallWatcher watcher(
                proxy.asyncCall(QStringLiteral("setDeviceEnabled"), wacom, false));
        QEventLoop loop;
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        if (!watcher.isFinished())
            loop.exec();
        ASSERT_TRUE(watcher.isFinished());
        const QDBusPendingReply<> reply = watcher;
        EXPECT_FALSE(reply.isError()) << reply.error().message().toStdString();
        EXPECT_EQ(proxy.property("disabledDevices").toStringList(), QStringList{wacom});
        EXPECT_FALSE(settings.deviceEnabled(wacom));
    }
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
