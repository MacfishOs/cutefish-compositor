// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/mouse_settings.h"

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

using cutefish::input::MouseSettings;

TEST(MouseSettings, RestoresLegacyPreferencesAndAccelerationMeaning)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    {
        QSettings legacy(file, QSettings::IniFormat);
        legacy.beginGroup(QStringLiteral("Mouse"));
        legacy.setValue(QStringLiteral("leftHanded"), true);
        legacy.setValue(QStringLiteral("naturalScroll"), true);
        legacy.setValue(QStringLiteral("accelerationProfile"), true);
        legacy.setValue(QStringLiteral("pointerAcceleration"), 0.6);
    }
    MouseSettings settings(nullptr, file);
    EXPECT_TRUE(settings.leftHanded());
    EXPECT_TRUE(settings.naturalScroll());
    EXPECT_FALSE(settings.acceleration());
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0.6);
    EXPECT_FALSE(settings.available());
}

TEST(MouseSettings, PersistsWithoutDevicesAndNotifiesOnlyOnChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    {
        MouseSettings settings(nullptr, file);
        EXPECT_TRUE(settings.acceleration());
        QSignalSpy changes(&settings, &MouseSettings::settingsChanged);
        settings.setLeftHanded(true);
        settings.setLeftHanded(true);
        settings.setNaturalScroll(true);
        settings.setAcceleration(false);
        settings.setPointerAcceleration(-0.8);
        EXPECT_EQ(changes.count(), 4);
        QSignalSpy availability(&settings, &MouseSettings::availableChanged);
        settings.setAvailable(true);
        settings.setAvailable(true);
        settings.setAvailable(false);
        EXPECT_EQ(availability.count(), 2);
        EXPECT_EQ(changes.count(), 4);
    }
    MouseSettings restored(nullptr, file);
    EXPECT_TRUE(restored.leftHanded());
    EXPECT_TRUE(restored.naturalScroll());
    EXPECT_FALSE(restored.acceleration());
    EXPECT_DOUBLE_EQ(restored.pointerAcceleration(), -0.8);
    EXPECT_FALSE(restored.available());
}

TEST(MouseSettings, RejectsNonFiniteSpeedAndClampsRange)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    MouseSettings settings(nullptr, file);
    QSignalSpy changes(&settings, &MouseSettings::settingsChanged);
    settings.setPointerAcceleration(std::numeric_limits<double>::quiet_NaN());
    settings.setPointerAcceleration(std::numeric_limits<double>::infinity());
    settings.setPointerAcceleration(-std::numeric_limits<double>::infinity());
    EXPECT_EQ(changes.count(), 0);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0);
    settings.setPointerAcceleration(10);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 1);
    settings.setPointerAcceleration(-10);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), -1);
    EXPECT_EQ(changes.count(), 2);
}

TEST(MouseSettings, SanitizesStoredSpeed)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    for (const auto &value :
         {QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("invalid")}) {
        {
            QSettings stored(file, QSettings::IniFormat);
            stored.beginGroup(QStringLiteral("Mouse"));
            stored.setValue(QStringLiteral("pointerAcceleration"), value);
        }
        MouseSettings settings(nullptr, file);
        EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0);
    }
    {
        QSettings stored(file, QSettings::IniFormat);
        stored.beginGroup(QStringLiteral("Mouse"));
        stored.setValue(QStringLiteral("pointerAcceleration"), 20);
    }
    MouseSettings settings(nullptr, file);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 1);
}

TEST(MouseSettings, DBusPropertiesMethodsAndServiceRestart)
{
    ASSERT_TRUE(QDBusConnection::sessionBus().isConnected());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    for (int iteration = 0; iteration < 2; ++iteration) {
        MouseSettings settings(nullptr, file);
        ASSERT_TRUE(settings.registerService());
        QDBusInterface proxy(QStringLiteral("com.cutefish.Mouse1"),
                             QStringLiteral("/com/cutefish/Mouse1"),
                             QStringLiteral("com.cutefish.Mouse1"));
        ASSERT_TRUE(proxy.isValid());
        EXPECT_EQ(proxy.property("leftHanded").toBool(), iteration != 0);
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
        invoke(QStringLiteral("setLeftHanded"), true);
        invoke(QStringLiteral("setNaturalScroll"), true);
        invoke(QStringLiteral("setAcceleration"), false);
        invoke(QStringLiteral("setPointerAcceleration"), 0.4);
        EXPECT_TRUE(proxy.property("leftHanded").toBool());
        EXPECT_TRUE(proxy.property("naturalScroll").toBool());
        EXPECT_FALSE(proxy.property("acceleration").toBool());
        EXPECT_DOUBLE_EQ(proxy.property("pointerAcceleration").toDouble(), 0.4);
        settings.setAvailable(true);
        EXPECT_TRUE(proxy.property("available").toBool());
    }
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
