// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/touchpad_settings.h"

#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>
#include <limits>

using cutefish::input::TouchpadSettings;

TEST(TouchpadSettings, DefaultsFollowChromiumAndLibinput)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TouchpadSettings settings(nullptr, directory.filePath(QStringLiteral("touchpad.conf")));
    EXPECT_FALSE(settings.available());
    EXPECT_TRUE(settings.enabled());
    EXPECT_FALSE(settings.disableWithMouse());
    EXPECT_TRUE(settings.disableWhileTyping());
    EXPECT_FALSE(settings.tapToClick());
    EXPECT_FALSE(settings.tapDragging());
    EXPECT_FALSE(settings.naturalScroll());
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0);
    EXPECT_DOUBLE_EQ(settings.scrollSpeed(), TouchpadSettings::kDefaultScrollSpeed);
    EXPECT_TRUE(settings.twoFingerSecondaryClick());
    EXPECT_FALSE(settings.secondaryClickConfigurable());
}

TEST(TouchpadSettings, PersistsWithoutDevicesAndNotifiesOnlyOnChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("touchpad.conf"));
    {
        TouchpadSettings settings(nullptr, file);
        QSignalSpy changes(&settings, &TouchpadSettings::settingsChanged);
        settings.setEnabled(false);
        settings.setEnabled(false);
        settings.setDisableWithMouse(true);
        settings.setDisableWhileTyping(false);
        settings.setTapToClick(true);
        settings.setTapDragging(true);
        settings.setNaturalScroll(true);
        settings.setPointerAcceleration(0.4);
        settings.setScrollSpeed(0.75);
        settings.setTwoFingerSecondaryClick(false);
        settings.setTwoFingerSecondaryClick(false);
        EXPECT_EQ(changes.count(), 9);
        QSignalSpy availability(&settings, &TouchpadSettings::availableChanged);
        settings.setAvailable(true);
        settings.setAvailable(true);
        settings.setAvailable(false);
        EXPECT_EQ(availability.count(), 2);
        QSignalSpy configurable(&settings,
                                &TouchpadSettings::secondaryClickConfigurableChanged);
        settings.setSecondaryClickConfigurable(true);
        settings.setSecondaryClickConfigurable(true);
        EXPECT_EQ(configurable.count(), 1);
        EXPECT_EQ(changes.count(), 9);
    }
    TouchpadSettings restored(nullptr, file);
    EXPECT_FALSE(restored.enabled());
    EXPECT_TRUE(restored.disableWithMouse());
    EXPECT_FALSE(restored.disableWhileTyping());
    EXPECT_TRUE(restored.tapToClick());
    EXPECT_TRUE(restored.tapDragging());
    EXPECT_TRUE(restored.naturalScroll());
    EXPECT_DOUBLE_EQ(restored.pointerAcceleration(), 0.4);
    EXPECT_DOUBLE_EQ(restored.scrollSpeed(), 0.75);
    EXPECT_FALSE(restored.twoFingerSecondaryClick());
    EXPECT_FALSE(restored.available());
    EXPECT_FALSE(restored.secondaryClickConfigurable());
}

TEST(TouchpadSettings, RejectsNonFiniteSpeedAndClampsRange)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("touchpad.conf"));
    {
        QSettings stored(file, QSettings::IniFormat);
        stored.setValue(QStringLiteral("pointerAcceleration"), QStringLiteral("nan"));
        stored.setValue(QStringLiteral("scrollSpeed"), QStringLiteral("inf"));
    }
    TouchpadSettings settings(nullptr, file);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0);
    EXPECT_DOUBLE_EQ(settings.scrollSpeed(), TouchpadSettings::kDefaultScrollSpeed);
    QSignalSpy changes(&settings, &TouchpadSettings::settingsChanged);
    settings.setPointerAcceleration(std::numeric_limits<double>::quiet_NaN());
    settings.setPointerAcceleration(std::numeric_limits<double>::infinity());
    EXPECT_EQ(changes.count(), 0);
    settings.setPointerAcceleration(10);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 1);
    settings.setPointerAcceleration(-10);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), -1);
    settings.setScrollSpeed(std::numeric_limits<double>::quiet_NaN());
    settings.setScrollSpeed(0);
    EXPECT_DOUBLE_EQ(settings.scrollSpeed(), TouchpadSettings::kMinimumScrollSpeed);
    settings.setScrollSpeed(100);
    EXPECT_DOUBLE_EQ(settings.scrollSpeed(), TouchpadSettings::kMaximumScrollSpeed);
    EXPECT_EQ(changes.count(), 4);
}

TEST(TouchpadSettings, DBusPropertiesMethodsAndServiceRestart)
{
    ASSERT_TRUE(QDBusConnection::sessionBus().isConnected());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("touchpad.conf"));
    for (int iteration = 0; iteration < 2; ++iteration) {
        TouchpadSettings settings(nullptr, file);
        ASSERT_TRUE(settings.registerService());
        QDBusInterface proxy(QStringLiteral("com.cutefish.Touchpad1"),
                             QStringLiteral("/com/cutefish/Touchpad1"),
                             QStringLiteral("com.cutefish.Touchpad1"));
        ASSERT_TRUE(proxy.isValid());
        EXPECT_EQ(proxy.property("tapToClick").toBool(), iteration != 0);
        const auto invoke = [&proxy](const QString &method, const QVariant &value) {
            QDBusPendingCallWatcher watcher(proxy.asyncCall(method, value));
            QEventLoop loop;
            QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop,
                             &QEventLoop::quit);
            QTimer::singleShot(2000, &loop, &QEventLoop::quit);
            if (!watcher.isFinished())
                loop.exec();
            EXPECT_TRUE(watcher.isFinished());
            const QDBusPendingReply<> reply = watcher;
            EXPECT_FALSE(reply.isError()) << reply.error().message().toStdString();
        };
        invoke(QStringLiteral("setEnabled"), false);
        invoke(QStringLiteral("setDisableWithMouse"), true);
        invoke(QStringLiteral("setDisableWhileTyping"), false);
        invoke(QStringLiteral("setTapToClick"), true);
        invoke(QStringLiteral("setTapDragging"), true);
        invoke(QStringLiteral("setNaturalScroll"), true);
        invoke(QStringLiteral("setPointerAcceleration"), -0.6);
        invoke(QStringLiteral("setScrollSpeed"), 1.25);
        invoke(QStringLiteral("setTwoFingerSecondaryClick"), false);
        EXPECT_FALSE(proxy.property("enabled").toBool());
        EXPECT_TRUE(proxy.property("disableWithMouse").toBool());
        EXPECT_FALSE(proxy.property("disableWhileTyping").toBool());
        EXPECT_TRUE(proxy.property("tapToClick").toBool());
        EXPECT_TRUE(proxy.property("tapDragging").toBool());
        EXPECT_TRUE(proxy.property("naturalScroll").toBool());
        EXPECT_DOUBLE_EQ(proxy.property("pointerAcceleration").toDouble(), -0.6);
        EXPECT_DOUBLE_EQ(proxy.property("scrollSpeed").toDouble(), 1.25);
        EXPECT_FALSE(proxy.property("twoFingerSecondaryClick").toBool());
        settings.setAvailable(true);
        EXPECT_TRUE(proxy.property("available").toBool());
        settings.setSecondaryClickConfigurable(true);
        EXPECT_TRUE(proxy.property("secondaryClickConfigurable").toBool());
    }
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
