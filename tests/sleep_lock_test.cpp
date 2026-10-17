// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fake_logind.h"
#include "loop_test_util.h"
#include "scene/node.h"
#include "session/session_controller.h"
#include "session/sleep_lock.h"

#include <gtest/gtest.h>

using namespace cutefish;

namespace {

class SleepLockTest : public testing::Test
{
protected:
    test::FakeLogind logind;
    // The service under test reaches logind on the session bus.
    base::dbus::Connection *bus = base::dbus::Connection::session(core::mainLoop());

    bool waitFor(const std::function<bool()> &done) { return test::runUntil(done); }
};

} // namespace

TEST_F(SleepLockTest, SleepWaitsForTheLockOnEveryOutput)
{
    ASSERT_TRUE(logind.isValid());
    scene::Node root;
    session::SecureContainer secure(&root);
    session::SessionController controller(&secure);
    controller.setOutputs({{"one", base::Rect(0, 0, 800, 600)},
                           {"two", base::Rect(800, 0, 800, 600)}});
    session::SleepLock lock(&controller, bus);
    ASSERT_TRUE(lock.start());
    ASSERT_TRUE(waitFor([&] { return lock.holdsInhibitor(); }));
    ASSERT_EQ(logind.lastInhibit.size(), 4u);
    EXPECT_EQ(logind.lastInhibit[0], "sleep");
    EXPECT_EQ(logind.lastInhibit[3], "delay");
    EXPECT_TRUE(logind.held());

    logind.prepareForSleep(true);
    EXPECT_TRUE(waitFor([&] { return controller.state() == session::SessionState::Locking; }));
    controller.notifyOutputPresented("one");
    test::runFor(std::chrono::milliseconds(20));
    EXPECT_TRUE(lock.holdsInhibitor());
    EXPECT_TRUE(logind.held());
    controller.notifyOutputPresented("two");
    EXPECT_EQ(controller.state(), session::SessionState::Locked);
    EXPECT_FALSE(lock.holdsInhibitor());
    EXPECT_TRUE(waitFor([&] { return !logind.held(); }));

    // Awake again: the next sleep is held back too.
    const size_t inhibits = logind.readEnds.size();
    logind.prepareForSleep(false);
    EXPECT_TRUE(waitFor([&] { return lock.holdsInhibitor(); }));
    EXPECT_EQ(logind.readEnds.size(), inhibits + 1);
    EXPECT_TRUE(logind.held());
}

TEST_F(SleepLockTest, AnUnlockInProgressIsLockedAgainBeforeSleeping)
{
    ASSERT_TRUE(logind.isValid());
    scene::Node root;
    session::SecureContainer secure(&root);
    session::SessionController controller(&secure);
    controller.setOutputs({{"one", base::Rect(0, 0, 800, 600)}});
    session::SleepLock lock(&controller, bus);
    ASSERT_TRUE(lock.start());
    ASSERT_TRUE(waitFor([&] { return lock.holdsInhibitor(); }));
    EXPECT_TRUE(controller.requestLock());
    controller.notifyOutputPresented("one");
    EXPECT_TRUE(controller.requestUnlock());
    EXPECT_EQ(controller.state(), session::SessionState::Unlocking);

    logind.prepareForSleep(true);
    test::runFor(std::chrono::milliseconds(50));
    EXPECT_EQ(controller.state(), session::SessionState::Unlocking);
    EXPECT_TRUE(lock.holdsInhibitor());
    controller.notifyOutputPresented("one");
    EXPECT_TRUE(waitFor([&] { return controller.state() == session::SessionState::Locking; }));
    EXPECT_TRUE(lock.holdsInhibitor());
    controller.notifyOutputPresented("one");
    EXPECT_EQ(controller.state(), session::SessionState::Locked);
    EXPECT_TRUE(waitFor([&] { return !logind.held(); }));
    logind.prepareForSleep(false);
    EXPECT_TRUE(waitFor([&] { return lock.holdsInhibitor(); }));
}

TEST_F(SleepLockTest, SleepWaitsWhileTheDisplayLayoutIsRebuilt)
{
    ASSERT_TRUE(logind.isValid());
    scene::Node root;
    session::SecureContainer secure(&root);
    session::SessionController controller(&secure);
    controller.setOutputs({{"one", base::Rect(0, 0, 800, 600)}});
    session::SleepLock lock(&controller, bus);
    ASSERT_TRUE(lock.start());
    ASSERT_TRUE(waitFor([&] { return lock.holdsInhibitor(); }));

    logind.prepareForSleep(true);
    EXPECT_TRUE(waitFor([&] { return controller.state() == session::SessionState::Locking; }));
    controller.setOutputs({});
    controller.notifyFramePresented();
    test::runFor(std::chrono::milliseconds(20));
    EXPECT_EQ(controller.state(), session::SessionState::Locking);
    EXPECT_TRUE(lock.holdsInhibitor());
    EXPECT_TRUE(logind.held());

    controller.setOutputs({{"one", base::Rect(0, 0, 800, 600)}});
    EXPECT_EQ(controller.state(), session::SessionState::Locking);
    controller.notifyOutputPresented("one");
    EXPECT_EQ(controller.state(), session::SessionState::Locked);
    EXPECT_TRUE(waitFor([&] { return !logind.held(); }));
}

TEST_F(SleepLockTest, AnAlreadyLockedSessionSleepsAtOnce)
{
    ASSERT_TRUE(logind.isValid());
    scene::Node root;
    session::SecureContainer secure(&root);
    session::SessionController controller(&secure);
    controller.setOutputs({{"one", base::Rect(0, 0, 800, 600)}});
    session::SleepLock lock(&controller, bus);
    ASSERT_TRUE(lock.start());
    ASSERT_TRUE(waitFor([&] { return lock.holdsInhibitor(); }));
    EXPECT_TRUE(controller.requestLock());
    controller.notifyOutputPresented("one");
    EXPECT_EQ(controller.state(), session::SessionState::Locked);

    logind.prepareForSleep(true);
    EXPECT_TRUE(waitFor([&] { return !logind.held(); }));
    EXPECT_FALSE(lock.holdsInhibitor());
    logind.prepareForSleep(false);
    EXPECT_TRUE(waitFor([&] { return lock.holdsInhibitor(); }));
}

TEST_F(SleepLockTest, LockedHintFollowsTheLock)
{
    ASSERT_TRUE(logind.isValid());
    scene::Node root;
    session::SecureContainer secure(&root);
    session::SessionController controller(&secure);
    controller.setOutputs({{"one", base::Rect(0, 0, 800, 600)}});
    session::SleepLock lock(&controller, bus);
    ASSERT_TRUE(lock.start());

    EXPECT_TRUE(controller.requestLock());
    test::runFor(std::chrono::milliseconds(20));
    EXPECT_TRUE(logind.lockedHints.empty());
    controller.notifyOutputPresented("one");
    ASSERT_TRUE(waitFor([&] { return logind.lockedHints.size() == 1; }));
    EXPECT_TRUE(logind.lockedHints[0]);

    EXPECT_TRUE(controller.requestUnlock());
    test::runFor(std::chrono::milliseconds(20));
    EXPECT_EQ(logind.lockedHints.size(), 1u);
    controller.notifyOutputPresented("one");
    ASSERT_TRUE(waitFor([&] { return logind.lockedHints.size() == 2; }));
    EXPECT_FALSE(logind.lockedHints[1]);

    test::runFor(std::chrono::milliseconds(20));
    EXPECT_EQ(logind.lockedHints.size(), 2u);
}
