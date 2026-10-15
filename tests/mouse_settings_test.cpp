// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/mouse_settings.h"

#include <gtest/gtest.h>
#include <limits>

#include "base/event_loop.h"
#include "base/settings.h"
#include "input_test_util.h"

using cutefish::base::EventLoop;
using cutefish::base::Settings;
using cutefish::input::MouseSettings;
using input_test::RemoteObject;
using input_test::TemporaryDirectory;

TEST(MouseSettings, RestoresLegacyPreferencesAndAccelerationMeaning)
{
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    const std::string file = directory.filePath("compositor.conf");
    {
        Settings legacy(file);
        legacy.setValue("Mouse/leftHanded", true);
        legacy.setValue("Mouse/naturalScroll", false);
        legacy.setValue("Mouse/accelerationProfile", true);
        legacy.setValue("Mouse/pointerAcceleration", 0.6);
    }
    MouseSettings settings(file);
    EXPECT_TRUE(settings.leftHanded());
    EXPECT_FALSE(settings.naturalScroll());
    EXPECT_FALSE(settings.acceleration());
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0.6);
    EXPECT_FALSE(settings.available());
}

TEST(MouseSettings, PersistsWithoutDevicesAndNotifiesOnlyOnChanges)
{
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    const std::string file = directory.filePath("compositor.conf");
    {
        MouseSettings settings(file);
        EXPECT_TRUE(settings.acceleration());
        EXPECT_TRUE(settings.naturalScroll());
        int changes = 0;
        auto counting = settings.settingsChanged.connect([&changes] { ++changes; });
        settings.setLeftHanded(true);
        settings.setLeftHanded(true);
        settings.setNaturalScroll(false);
        settings.setAcceleration(false);
        settings.setPointerAcceleration(-0.8);
        EXPECT_EQ(changes, 4);
        int availability = 0;
        auto counted = settings.availableChanged.connect([&availability] { ++availability; });
        settings.setAvailable(true);
        settings.setAvailable(true);
        settings.setAvailable(false);
        EXPECT_EQ(availability, 2);
        EXPECT_EQ(changes, 4);
    }
    MouseSettings restored(file);
    EXPECT_TRUE(restored.leftHanded());
    EXPECT_FALSE(restored.naturalScroll());
    EXPECT_FALSE(restored.acceleration());
    EXPECT_DOUBLE_EQ(restored.pointerAcceleration(), -0.8);
    EXPECT_FALSE(restored.available());
}

TEST(MouseSettings, WritesTheKeysSettingsReads)
{
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    const std::string file = directory.filePath("compositor.conf");
    {
        MouseSettings settings(file);
        settings.setLeftHanded(true);
        settings.setAcceleration(false);
        settings.setPointerAcceleration(0.25);
    }
    Settings stored(file);
    EXPECT_EQ(stored.rawValue("Mouse/leftHanded"), "true");
    EXPECT_EQ(stored.rawValue("Mouse/accelerationProfile"), "true");
    EXPECT_EQ(stored.rawValue("Mouse/pointerAcceleration"), "0.25");
}

TEST(MouseSettings, RejectsNonFiniteSpeedAndClampsRange)
{
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    MouseSettings settings(directory.filePath("compositor.conf"));
    int changes = 0;
    auto counting = settings.settingsChanged.connect([&changes] { ++changes; });
    settings.setPointerAcceleration(std::numeric_limits<double>::quiet_NaN());
    settings.setPointerAcceleration(std::numeric_limits<double>::infinity());
    settings.setPointerAcceleration(-std::numeric_limits<double>::infinity());
    EXPECT_EQ(changes, 0);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0);
    settings.setPointerAcceleration(10);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 1);
    settings.setPointerAcceleration(-10);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), -1);
    EXPECT_EQ(changes, 2);
}

TEST(MouseSettings, SanitizesStoredSpeed)
{
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    const std::string file = directory.filePath("compositor.conf");
    for (const char *value : {"nan", "inf", "invalid"}) {
        {
            Settings stored(file);
            stored.setValue("Mouse/pointerAcceleration", value);
        }
        MouseSettings settings(file);
        EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 0);
    }
    {
        Settings stored(file);
        stored.setValue("Mouse/pointerAcceleration", 20);
    }
    MouseSettings settings(file);
    EXPECT_DOUBLE_EQ(settings.pointerAcceleration(), 1);
}

TEST(MouseSettings, DBusPropertiesMethodsAndServiceRestart)
{
    EventLoop loop;
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    const std::string file = directory.filePath("compositor.conf");
    for (int iteration = 0; iteration < 2; ++iteration) {
        MouseSettings settings(file);
        ASSERT_TRUE(settings.registerService());
        RemoteObject proxy(loop, "com.cutefish.Mouse1", "/com/cutefish/Mouse1",
                           "com.cutefish.Mouse1");
        ASSERT_TRUE(proxy.isValid());
        EXPECT_EQ(proxy.property<bool>("leftHanded"), iteration != 0);
        const auto invoke = [&proxy](const std::string &method, auto value) {
            const auto reply = proxy.call(method, value);
            EXPECT_FALSE(reply.isError()) << method << ": " << reply.error().message;
        };
        invoke("setLeftHanded", true);
        invoke("setNaturalScroll", false);
        invoke("setAcceleration", false);
        invoke("setPointerAcceleration", 0.4);
        EXPECT_EQ(proxy.property<bool>("leftHanded"), true);
        EXPECT_EQ(proxy.property<bool>("naturalScroll"), false);
        EXPECT_EQ(proxy.property<bool>("acceleration"), false);
        EXPECT_EQ(proxy.property<double>("pointerAcceleration"), 0.4);
        settings.setAvailable(true);
        EXPECT_EQ(proxy.property<bool>("available"), true);
    }
}

TEST(MouseSettings, SendsTheChangeSignalsSettingsListensTo)
{
    EventLoop loop;
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    MouseSettings settings(directory.filePath("compositor.conf"));
    ASSERT_TRUE(settings.registerService());
    RemoteObject proxy(loop, "com.cutefish.Mouse1", "/com/cutefish/Mouse1", "com.cutefish.Mouse1");
    ASSERT_TRUE(proxy.isValid());
    std::vector<std::string> received;
    cutefish::base::dbus::MatchRule rule;
    rule.path = "/com/cutefish/Mouse1";
    rule.interface = "com.cutefish.Mouse1";
    auto match = proxy.connection()->subscribe(rule, [&received](const auto &message) {
        received.push_back(message.member() + "(" + message.signature() + ")");
    });
    // A round trip puts the match rule in place before the signals go out.
    EXPECT_EQ(proxy.property<bool>("leftHanded"), false);
    proxy.call("setLeftHanded", true);
    EXPECT_TRUE(input_test::runUntil(loop, [&received] { return received.size() == 2; }));
    EXPECT_EQ(received, (std::vector<std::string> {"settingsChanged()", "leftHandedChanged()"}));
}
