// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/keyboard_settings.h"

#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEventLoop>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>

using cutefish::input::KeyboardSettings;
using cutefish::input::ShortcutConvention;

TEST(KeyboardSettings, DefaultsAndClampedStoredValues)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    {
        KeyboardSettings settings(nullptr, file);
        EXPECT_TRUE(settings.repeatEnabled());
        EXPECT_EQ(settings.repeatDelay(), 500);
        EXPECT_EQ(settings.repeatInterval(), 40);
        EXPECT_FALSE(settings.layout().isEmpty());
    }
    {
        QSettings stored(file, QSettings::IniFormat);
        stored.beginGroup(QStringLiteral("Keyboard"));
        stored.setValue(QStringLiteral("repeatDelay"), 1);
        stored.setValue(QStringLiteral("repeatInterval"), QStringLiteral("fast"));
    }
    KeyboardSettings settings(nullptr, file);
    EXPECT_EQ(settings.repeatDelay(), KeyboardSettings::kMinRepeatDelayMs);
    EXPECT_EQ(settings.repeatInterval(), 40);
}

TEST(KeyboardSettings, RepeatPersistsAndNotifiesOnlyOnChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    {
        KeyboardSettings settings(nullptr, file);
        QSignalSpy delay(&settings, &KeyboardSettings::repeatDelayChanged);
        QSignalSpy interval(&settings, &KeyboardSettings::repeatIntervalChanged);
        QSignalSpy enabled(&settings, &KeyboardSettings::repeatEnabledChanged);
        settings.setRepeatDelay(300);
        settings.setRepeatDelay(300);
        settings.setRepeatInterval(100000);
        settings.setRepeatEnabled(false);
        EXPECT_EQ(delay.count(), 1);
        EXPECT_EQ(interval.count(), 1);
        EXPECT_EQ(enabled.count(), 1);
        EXPECT_EQ(settings.repeatInterval(), KeyboardSettings::kMaxRepeatIntervalMs);
    }
    KeyboardSettings restored(nullptr, file);
    EXPECT_FALSE(restored.repeatEnabled());
    EXPECT_EQ(restored.repeatDelay(), 300);
    EXPECT_EQ(restored.repeatInterval(), KeyboardSettings::kMaxRepeatIntervalMs);
}

TEST(KeyboardSettings, LayoutIsSavedOnlyWhenItApplies)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    QStringList applied;
    const auto applier = [&applied](const QString &name) {
        if (name == QLatin1String("broken"))
            return false;
        applied << name;
        return true;
    };
    {
        KeyboardSettings settings(nullptr, file);
        const QString fallback = settings.layout();
        EXPECT_FALSE(settings.setLayout(QStringLiteral("de")));
        settings.setLayoutApplier(applier);
        // Nothing saved, so nothing is installed up front.
        EXPECT_TRUE(applied.isEmpty());

        QSignalSpy changes(&settings, &KeyboardSettings::layoutChanged);
        EXPECT_FALSE(settings.setLayout(QStringLiteral("broken")));
        EXPECT_EQ(settings.layout(), fallback);
        EXPECT_TRUE(settings.setLayout(QStringLiteral("de-neo")));
        EXPECT_TRUE(settings.setLayout(QStringLiteral("de-neo")));
        EXPECT_EQ(changes.count(), 1);
        EXPECT_EQ(settings.layout(), QStringLiteral("de-neo"));
    }
    applied.clear();
    {
        KeyboardSettings restored(nullptr, file);
        restored.setLayoutApplier(applier);
        EXPECT_EQ(applied, QStringList {QStringLiteral("de-neo")});
        EXPECT_TRUE(restored.setLayout(QString()));
        EXPECT_NE(restored.layout(), QStringLiteral("de-neo"));
    }
    QSettings stored(file, QSettings::IniFormat);
    stored.beginGroup(QStringLiteral("Keyboard"));
    EXPECT_FALSE(stored.contains(QStringLiteral("layout")));
}

TEST(KeyboardSettings, ASavedLayoutThatNoLongerAppliesFallsBack)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    {
        QSettings stored(file, QSettings::IniFormat);
        stored.beginGroup(QStringLiteral("Keyboard"));
        stored.setValue(QStringLiteral("layout"), QStringLiteral("broken"));
    }
    KeyboardSettings settings(nullptr, file);
    EXPECT_EQ(settings.layout(), QStringLiteral("broken"));
    QSignalSpy changes(&settings, &KeyboardSettings::layoutChanged);
    settings.setLayoutApplier([](const QString &) {
        return false;
    });
    EXPECT_EQ(changes.count(), 1);
    EXPECT_NE(settings.layout(), QStringLiteral("broken"));
}

TEST(KeyboardSettings, TheShortcutConventionIsSavedAndStartsAsPc)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("compositor.conf"));
    {
        KeyboardSettings settings(nullptr, file);
        EXPECT_EQ(settings.shortcutConvention(), ShortcutConvention::Pc);
        QSignalSpy changes(&settings, &KeyboardSettings::shortcutConventionChanged);
        EXPECT_FALSE(settings.setShortcutConvention(QStringLiteral("mac")));
        EXPECT_TRUE(settings.setShortcutConvention(QStringLiteral("command")));
        EXPECT_TRUE(settings.setShortcutConvention(QStringLiteral("command")));
        EXPECT_EQ(changes.count(), 1);
    }
    {
        KeyboardSettings restored(nullptr, file);
        EXPECT_EQ(restored.shortcutConvention(), ShortcutConvention::Command);
        EXPECT_EQ(restored.shortcutConventionName(), QStringLiteral("command"));
        EXPECT_TRUE(restored.setShortcutConvention(QStringLiteral("pc")));
    }
    QSettings stored(file, QSettings::IniFormat);
    stored.beginGroup(QStringLiteral("Keyboard"));
    EXPECT_FALSE(stored.contains(QStringLiteral("shortcutConvention")));
}

TEST(KeyboardSettings, DBusPropertiesAndMethods)
{
    ASSERT_TRUE(QDBusConnection::sessionBus().isConnected());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    KeyboardSettings settings(nullptr, directory.filePath(QStringLiteral("compositor.conf")));
    settings.setLayoutApplier([](const QString &name) {
        return name != QLatin1String("broken");
    });
    ASSERT_TRUE(settings.registerService());
    QDBusInterface proxy(QStringLiteral("com.cutefish.Keyboard1"),
                         QStringLiteral("/com/cutefish/Keyboard1"),
                         QStringLiteral("com.cutefish.Keyboard1"));
    ASSERT_TRUE(proxy.isValid());
    const auto invoke = [&proxy](const QString &method, const QVariant &value) {
        QDBusPendingCallWatcher watcher(proxy.asyncCall(method, value));
        QEventLoop loop;
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        if (!watcher.isFinished())
            loop.exec();
        EXPECT_TRUE(watcher.isFinished());
        const QDBusPendingReply<> reply = watcher;
        EXPECT_FALSE(reply.isError()) << reply.error().message().toStdString();
        return watcher.reply().arguments().value(0);
    };
    invoke(QStringLiteral("setRepeatEnabled"), false);
    invoke(QStringLiteral("setRepeatDelay"), 250);
    invoke(QStringLiteral("setRepeatInterval"), 25);
    EXPECT_FALSE(invoke(QStringLiteral("setLayout"), QStringLiteral("broken")).toBool());
    EXPECT_TRUE(invoke(QStringLiteral("setLayout"), QStringLiteral("fr")).toBool());
    EXPECT_TRUE(
            invoke(QStringLiteral("setShortcutConvention"), QStringLiteral("command")).toBool());
    EXPECT_FALSE(proxy.property("repeatEnabled").toBool());
    EXPECT_EQ(proxy.property("repeatDelay").toInt(), 250);
    EXPECT_EQ(proxy.property("repeatInterval").toInt(), 25);
    EXPECT_EQ(proxy.property("layout").toString(), QStringLiteral("fr"));
    EXPECT_EQ(proxy.property("shortcutConvention").toString(), QStringLiteral("command"));
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
