// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The pause on real input follows
// chromium/src/remoting/host/remote_input_filter_unittest.cc.
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65

#include <QString>
#include <algorithm>
#include <gtest/gtest.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "compositor/compositor.h"
#include "computer_control/controller.h"
#include "input/event_sink.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/node.h"
#include "scene/scene.h"

using namespace cutefish;
using namespace cutefish::computer_control;
using namespace std::chrono_literals;

namespace {

// Everything that reaches the window manager, in order.
class RecordingSink : public input::EventSink
{
public:
    std::vector<std::string> events;

    bool onPointerMotion(input::Timestamp, const QPointF &location) override
    {
        events.push_back("motion " + QString::number(location.x()).toStdString() + ","
                         + QString::number(location.y()).toStdString());
        return false;
    }

    bool onPointerButton(input::Timestamp, const QPointF &, uint32_t button, bool pressed) override
    {
        events.push_back("button " + std::to_string(button) + (pressed ? " down" : " up"));
        return false;
    }

    bool onPointerAxis(input::Timestamp, const QPointF &, const QPointF &offset, bool) override
    {
        events.push_back("axis " + QString::number(offset.x()).toStdString() + ","
                         + QString::number(offset.y()).toStdString());
        return false;
    }

    bool onKey(input::Timestamp, uint32_t key, bool pressed) override
    {
        events.push_back("key " + std::to_string(key) + (pressed ? " down" : " up"));
        return false;
    }
};

class FakeClient : public ControlClient
{
public:
    int granteds = 0;
    std::vector<RevokeReason> revokes;
    std::vector<Rejection> rejections;
    // Handle and title of each window event since the last windows_done.
    std::vector<std::pair<uint32_t, QString>> windows;
    int windowsDones = 0;
    std::vector<QPointF> pointers;

    void granted() override { ++granteds; }
    void revoked(RevokeReason reason) override { revokes.push_back(reason); }
    void rejected(Rejection rejection) override { rejections.push_back(rejection); }

    void window(uint32_t handle, const WindowInfo &window) override
    {
        windows.emplace_back(handle, window.title);
    }

    void windowsDone() override { ++windowsDones; }
    void pointer(const QPointF &position) override { pointers.push_back(position); }
};

std::string key(uint32_t code, bool pressed)
{
    return "key " + std::to_string(code) + (pressed ? " down" : " up");
}

class ComputerControlTest : public ::testing::Test
{
protected:
    scene::Scene scene;
    scene::Node *surfaces =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    compositor::Compositor compositor {surfaces};
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    RecordingSink sink;

    input::Timestamp clock = input::Timestamp(100s);
    std::function<void(ConsentResult)> answer;
    int questions = 0;
    int withdrawals = 0;
    bool protectedPoint = false;
    bool protectedFocus = false;
    bool textInputFocused = false;
    bool composing = false;
    bool blocked = false;
    bool commitAccepted = true;
    std::vector<std::string> commits;
    std::vector<WindowInfo> windows;
    std::vector<quint64> activations;
    std::optional<quint64> protectedWindow;
    std::vector<quint64> closes;
    std::vector<std::pair<quint64, QRect>> frames;
    std::vector<std::string> clipboards;
    std::vector<bool> activeChanges;
    std::unique_ptr<Controller> controller;

    void SetUp() override
    {
        scene.setSize(QSize(800, 600));
        input.setBounds(QSize(800, 600));
        input.setEventSink(&sink);

        Environment environment;
        environment.askConsent = [this](const QString &, std::function<void(ConsentResult)> reply) {
            ++questions;
            answer = std::move(reply);
            return [this] {
                ++withdrawals;
            };
        };
        environment.protectedAt = [this](const QPointF &) {
            return protectedPoint;
        };
        environment.protectedFocus = [this] {
            return protectedFocus;
        };
        environment.textInputFocused = [this] {
            return textInputFocused;
        };
        environment.inputMethodGrabsKeyboard = [this] {
            return composing;
        };
        environment.commitText = [this](const std::string &text) {
            if (!commitAccepted)
                return false;
            commits.push_back(text);
            return true;
        };
        environment.sessionBlocked = [this] {
            return blocked;
        };
        environment.now = [this] {
            return clock;
        };
        environment.windows = [this] {
            return windows;
        };
        environment.activateWindow = [this](quint64 id) {
            for (const WindowInfo &window : windows) {
                if (window.id == id) {
                    activations.push_back(id);
                    return true;
                }
            }
            return false;
        };
        const auto listed = [this](quint64 id) {
            return std::any_of(windows.begin(), windows.end(), [id](const WindowInfo &window) {
                return window.id == id;
            });
        };
        environment.protectedWindow = [this](quint64 id) {
            return protectedWindow == id;
        };
        environment.closeWindow = [this, listed](quint64 id) {
            if (!listed(id))
                return false;
            closes.push_back(id);
            return true;
        };
        environment.setWindowFrame = [this, listed](quint64 id, const QRect &frame) {
            if (!listed(id))
                return false;
            frames.emplace_back(id, frame);
            return true;
        };
        environment.setClipboard = [this](const std::string &text) {
            clipboards.push_back(text);
        };
        controller = std::make_unique<Controller>(&input, std::move(environment));
        QObject::connect(controller.get(), &Controller::activeChanged, [this](bool active) {
            activeChanges.push_back(active);
        });
    }

    std::unique_ptr<Control> grant(FakeClient &client)
    {
        auto control = controller->request(&client, &client, QStringLiteral("Agent"));
        EXPECT_TRUE(answer);
        std::exchange(answer, {})(ConsentResult::Allowed);
        EXPECT_EQ(1, client.granteds);
        sink.events.clear();
        return control;
    }
};

} // namespace

TEST_F(ComputerControlTest, NothingIsInjectedBeforeTheUserAgrees)
{
    FakeClient client;
    auto control = controller->request(&client, &client, QStringLiteral("Agent"));
    EXPECT_EQ(1, questions);
    EXPECT_FALSE(controller->isActive());

    control->motion(QPointF(10, 10));
    control->key(KEY_A, true);
    EXPECT_EQ((std::vector<Rejection> {Rejection::NotGranted, Rejection::NotGranted}),
              client.rejections);
    EXPECT_TRUE(sink.events.empty());

    answer(ConsentResult::Allowed);
    EXPECT_EQ(1, client.granteds);
    EXPECT_TRUE(controller->isActive());
    EXPECT_EQ(QStringLiteral("Agent"), controller->agentName());
    EXPECT_EQ(std::vector<bool> {true}, activeChanges);

    control->motion(QPointF(10, 20));
    EXPECT_EQ(std::vector<std::string> {"motion 10,20"}, sink.events);
    EXPECT_EQ(QPointF(10, 20), input.pointerPosition());
}

TEST_F(ComputerControlTest, ARefusalRevokes)
{
    FakeClient denied;
    auto control = controller->request(&denied, &denied, QStringLiteral("Agent"));
    answer(ConsentResult::Denied);
    EXPECT_EQ(std::vector<RevokeReason> {RevokeReason::Denied}, denied.revokes);
    EXPECT_FALSE(controller->isActive());
    EXPECT_TRUE(activeChanges.empty());

    FakeClient unavailable;
    auto other = controller->request(&unavailable, &unavailable, QStringLiteral("Agent"));
    answer(ConsentResult::Unavailable);
    EXPECT_EQ(std::vector<RevokeReason> {RevokeReason::Unavailable}, unavailable.revokes);
}

TEST_F(ComputerControlTest, OneControlAtATime)
{
    FakeClient first;
    auto control = controller->request(&first, &first, QStringLiteral("Agent"));
    FakeClient second;
    auto other = controller->request(&second, &second, QStringLiteral("Other"));
    EXPECT_EQ(std::vector<RevokeReason> {RevokeReason::Busy}, second.revokes);
    EXPECT_EQ(1, questions);

    // A withdrawn request frees the seat for the next one.
    control.reset();
    EXPECT_EQ(1, withdrawals);
    FakeClient third;
    auto last = controller->request(&third, &third, QStringLiteral("Agent"));
    EXPECT_TRUE(third.revokes.empty());
    EXPECT_EQ(2, questions);
}

TEST_F(ComputerControlTest, ABlockedSessionEndsControl)
{
    blocked = true;
    FakeClient refused;
    auto control = controller->request(&refused, &refused, QStringLiteral("Agent"));
    EXPECT_EQ(std::vector<RevokeReason> {RevokeReason::Locked}, refused.revokes);
    EXPECT_EQ(0, questions);

    blocked = false;
    FakeClient pending;
    auto asking = controller->request(&pending, &pending, QStringLiteral("Agent"));
    controller->sessionBlocked();
    EXPECT_EQ(std::vector<RevokeReason> {RevokeReason::Locked}, pending.revokes);
    EXPECT_EQ(1, withdrawals);

    FakeClient client;
    auto granted = grant(client);
    granted->key(KEY_LEFTSHIFT, true);
    // A lock the controller has not heard of yet ends it before the next event.
    blocked = true;
    granted->key(KEY_A, true);
    EXPECT_EQ(std::vector<RevokeReason> {RevokeReason::Locked}, client.revokes);
    EXPECT_EQ(std::vector<Rejection> {Rejection::NotGranted}, client.rejections);
    EXPECT_EQ((std::vector<bool> {true, false}), activeChanges);
}

TEST_F(ComputerControlTest, StoppingReleasesWhatTheAgentHolds)
{
    FakeClient client;
    auto control = grant(client);
    control->key(KEY_LEFTCTRL, true);
    control->button(BTN_LEFT, true);
    sink.events.clear();

    controller->stop();
    EXPECT_EQ(std::vector<RevokeReason> {RevokeReason::Stopped}, client.revokes);
    EXPECT_EQ((std::vector<std::string> {"button " + std::to_string(BTN_LEFT) + " up",
                                         key(KEY_LEFTCTRL, false)}),
              sink.events);
    EXPECT_TRUE(seat.pressedKeys().empty());
    EXPECT_EQ((std::vector<bool> {true, false}), activeChanges);

    // Releases after the end are no-ops, presses are refused.
    sink.events.clear();
    control->key(KEY_LEFTCTRL, false);
    control->key(KEY_A, true);
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(std::vector<Rejection> {Rejection::NotGranted}, client.rejections);
}

TEST_F(ComputerControlTest, RealInputPausesTheAgentAndReleasesWhatItHolds)
{
    FakeClient client;
    auto control = grant(client);
    control->key(KEY_LEFTSHIFT, true);
    control->button(BTN_LEFT, true);
    sink.events.clear();

    input.onPointerMotion(clock, QPointF(1, 0));
    ASSERT_EQ(3u, sink.events.size());
    EXPECT_EQ("button " + std::to_string(BTN_LEFT) + " up", sink.events[0]);
    EXPECT_EQ(key(KEY_LEFTSHIFT, false), sink.events[1]);
    EXPECT_TRUE(seat.pressedKeys().empty());

    sink.events.clear();
    control->motion(QPointF(50, 50));
    control->key(KEY_A, true);
    EXPECT_EQ((std::vector<Rejection> {Rejection::Paused, Rejection::Paused}), client.rejections);
    EXPECT_TRUE(sink.events.empty());

    clock += Controller::kPauseAfterInput - 1ms;
    control->key(KEY_A, true);
    EXPECT_EQ(3u, client.rejections.size());

    clock += 1ms;
    control->key(KEY_A, true);
    control->key(KEY_A, false);
    EXPECT_EQ(3u, client.rejections.size());
    EXPECT_EQ((std::vector<std::string> {key(KEY_A, true), key(KEY_A, false)}), sink.events);
    // Still in control: a pause is not a revocation.
    EXPECT_TRUE(client.revokes.empty());
}

TEST_F(ComputerControlTest, WhatTheUserStillHoldsPausesTheAgent)
{
    FakeClient client;
    auto control = grant(client);
    input.onKey(clock, KEY_LEFTMETA, true);
    clock += 10s;

    control->key(KEY_A, true);
    EXPECT_EQ(std::vector<Rejection> {Rejection::Paused}, client.rejections);

    input.onKey(clock, KEY_LEFTMETA, false);
    clock += 10s;
    sink.events.clear();
    control->key(KEY_A, true);
    EXPECT_EQ(1u, client.rejections.size());
    EXPECT_EQ(std::vector<std::string> {key(KEY_A, true)}, sink.events);
    control->key(KEY_A, false);
}

TEST_F(ComputerControlTest, SystemDialogsAreTheUsersToAnswer)
{
    FakeClient client;
    auto control = grant(client);
    control->key(KEY_TAB, true);

    protectedPoint = true;
    protectedFocus = true;
    control->motion(QPointF(30, 30));
    control->button(BTN_LEFT, true);
    control->axis(0, 1);
    control->key(KEY_ENTER, true);
    control->typeText("a");
    EXPECT_EQ(std::vector<Rejection>(4, Rejection::Protected), client.rejections);
    // Moving is allowed, and so is letting go of what was pressed before.
    control->key(KEY_TAB, false);
    EXPECT_EQ((std::vector<std::string> {key(KEY_TAB, true), "motion 30,30", key(KEY_TAB, false)}),
              sink.events);
}

TEST_F(ComputerControlTest, ScrollingIsInWheelClicks)
{
    FakeClient client;
    auto control = grant(client);
    control->axis(0, 2);
    control->axis(-1, 0);
    EXPECT_EQ((std::vector<std::string> {"axis 0,20", "axis -10,0"}), sink.events);
}

TEST_F(ComputerControlTest, InvalidCodesAndRepeatsAreIgnored)
{
    FakeClient client;
    auto control = grant(client);
    control->button(KEY_A, true);
    control->key(BTN_LEFT, true);
    control->key(KEY_RESERVED, true);
    control->key(KEY_A, true);
    control->key(KEY_A, true);
    control->key(KEY_B, false);
    EXPECT_EQ(std::vector<std::string> {key(KEY_A, true)}, sink.events);
    EXPECT_TRUE(client.rejections.empty());
    control->key(KEY_A, false);
}

TEST_F(ComputerControlTest, TextIsTypedOnTheKeymap)
{
    FakeClient client;
    auto control = grant(client);
    control->typeText("aA!\r\n\t");
    EXPECT_EQ((std::vector<std::string> {
                      key(KEY_A, true), key(KEY_A, false),
                      key(KEY_LEFTSHIFT, true), key(KEY_A, true), key(KEY_A, false),
                      key(KEY_LEFTSHIFT, false),
                      key(KEY_LEFTSHIFT, true), key(KEY_1, true), key(KEY_1, false),
                      key(KEY_LEFTSHIFT, false),
                      key(KEY_ENTER, true), key(KEY_ENTER, false),
                      key(KEY_TAB, true), key(KEY_TAB, false),
              }),
              sink.events);
    EXPECT_TRUE(commits.empty());
    EXPECT_TRUE(seat.pressedKeys().empty());
}

TEST_F(ComputerControlTest, AModifierTheAgentHoldsStaysHeld)
{
    FakeClient client;
    auto control = grant(client);
    control->key(KEY_LEFTSHIFT, true);
    sink.events.clear();
    control->typeText("A");
    EXPECT_EQ((std::vector<std::string> {key(KEY_A, true), key(KEY_A, false)}), sink.events);
    EXPECT_TRUE(seat.pressedKeys().contains(KEY_LEFTSHIFT));
    control->key(KEY_LEFTSHIFT, false);
}

TEST_F(ComputerControlTest, AKeysymPressesTheKeyOfTheLayout)
{
    FakeClient client;
    auto control = grant(client);
    ASSERT_TRUE(seat.setKeyboardLayout("de"));
    control->key(KEY_LEFTCTRL, true);
    control->keysym(XKB_KEY_z, true);
    // Released as pressed, whatever the layout is by then.
    ASSERT_TRUE(seat.setKeyboardLayout("us"));
    control->keysym(XKB_KEY_z, false);
    control->key(KEY_LEFTCTRL, false);
    EXPECT_EQ((std::vector<std::string> {
                      key(KEY_LEFTCTRL, true), key(KEY_Y, true), key(KEY_Y, false),
                      key(KEY_LEFTCTRL, false),
              }),
              sink.events);

    // The level's own modifier is the caller's to press.
    sink.events.clear();
    control->keysym(XKB_KEY_exclam, true);
    control->keysym(XKB_KEY_exclam, false);
    EXPECT_EQ((std::vector<std::string> {key(KEY_1, true), key(KEY_1, false)}), sink.events);

    control->keysym(XKB_KEY_Greek_alpha, true);
    EXPECT_EQ(std::vector<Rejection> {Rejection::Untypable}, client.rejections);
    EXPECT_TRUE(seat.pressedKeys().empty());
}

TEST_F(ComputerControlTest, TextBeyondTheKeymapIsCommitted)
{
    FakeClient client;
    auto control = grant(client);
    control->typeText("aé");
    EXPECT_EQ(std::vector<Rejection> {Rejection::Untypable}, client.rejections);
    EXPECT_TRUE(sink.events.empty());

    // All of it, since a commit would overtake keys sent before it.
    textInputFocused = true;
    control->typeText("aéü b");
    EXPECT_EQ((std::vector<std::string> {"aéü b"}), commits);
    EXPECT_TRUE(sink.events.empty());

    // The text-input refuses what it cannot send, such as invalid UTF-8.
    commitAccepted = false;
    control->typeText("é");
    EXPECT_EQ((std::vector<Rejection> {Rejection::Untypable, Rejection::Untypable}),
              client.rejections);
    commitAccepted = true;

    // A line break cannot be committed, so the caller sends the parts apart.
    client.rejections.clear();
    commits.clear();
    control->typeText("é\nb");
    EXPECT_EQ(std::vector<Rejection> {Rejection::Untypable}, client.rejections);
    EXPECT_TRUE(commits.empty());
    EXPECT_TRUE(sink.events.empty());

    // Keys would feed a composing input method, so everything is committed.
    composing = true;
    client.rejections.clear();
    control->typeText("ab");
    control->typeText("ab\n");
    EXPECT_EQ((std::vector<std::string> {"ab"}), commits);
    EXPECT_EQ(std::vector<Rejection> {Rejection::Untypable}, client.rejections);
    EXPECT_TRUE(sink.events.empty());
}

TEST_F(ComputerControlTest, ControlCharactersOtherThanLineBreaksAndTabsAreRefused)
{
    FakeClient client;
    auto control = grant(client);
    textInputFocused = true;
    control->typeText("a\x01");
    control->typeText("\x7f");
    EXPECT_EQ((std::vector<Rejection> {Rejection::Untypable, Rejection::Untypable}),
              client.rejections);
    EXPECT_TRUE(sink.events.empty());
    EXPECT_TRUE(commits.empty());
}

TEST_F(ComputerControlTest, OnlyTheGrantedClientMayCapture)
{
    FakeClient client;
    int other = 0;
    auto control = controller->request(&client, &client, QStringLiteral("Agent"));
    EXPECT_FALSE(controller->allowsCapture(&client));
    answer(ConsentResult::Allowed);
    EXPECT_TRUE(controller->allowsCapture(&client));
    EXPECT_FALSE(controller->allowsCapture(&other));
    EXPECT_FALSE(controller->allowsCapture(nullptr));
    control.reset();
    EXPECT_FALSE(controller->allowsCapture(&client));
    EXPECT_EQ((std::vector<bool> {true, false}), activeChanges);
}

TEST_F(ComputerControlTest, AControlOutlivingTheControllerRefusesEverything)
{
    FakeClient client;
    auto control = grant(client);
    control->key(KEY_A, true);
    controller.reset();
    sink.events.clear();

    control->key(KEY_A, false);
    control->motion(QPointF(1, 1));
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(std::vector<Rejection> {Rejection::NotGranted}, client.rejections);
    control.reset();
}

TEST_F(ComputerControlTest, WindowsAreListedWhileGranted)
{
    windows = {{7, QStringLiteral("gedit"), QStringLiteral("Notes"), QRect(10, 20, 300, 200),
                WindowInfo::Active},
               {3, QStringLiteral("firefox"), QStringLiteral("Mail"), QRect(), 0}};
    FakeClient client;
    auto control = controller->request(&client, &client, QStringLiteral("Agent"));
    control->listWindows();
    control->queryPointer();
    EXPECT_EQ((std::vector<Rejection> {Rejection::NotGranted, Rejection::NotGranted}),
              client.rejections);
    EXPECT_TRUE(client.windows.empty());
    EXPECT_EQ(0, client.windowsDones);
    EXPECT_TRUE(client.pointers.empty());

    answer(ConsentResult::Allowed);
    control->listWindows();
    using Listed = std::vector<std::pair<uint32_t, QString>>;
    EXPECT_EQ((Listed {{1, QStringLiteral("Notes")}, {2, QStringLiteral("Mail")}}),
              client.windows);
    EXPECT_EQ(1, client.windowsDones);

    // A window keeps its handle in a later list, in whatever order.
    windows = {windows[1], {9, QString(), QStringLiteral("New"), QRect(), 0}, windows[0]};
    client.windows.clear();
    control->listWindows();
    EXPECT_EQ((Listed {{2, QStringLiteral("Mail")}, {3, QStringLiteral("New")},
                       {1, QStringLiteral("Notes")}}),
              client.windows);

    control->motion(QPointF(40, 30));
    control->queryPointer();
    EXPECT_EQ(std::vector<QPointF> {QPointF(40, 30)}, client.pointers);
    EXPECT_EQ(2u, client.rejections.size());
}

TEST_F(ComputerControlTest, AWindowIsActivatedByItsHandle)
{
    windows = {{7, QStringLiteral("gedit"), QStringLiteral("Notes"), QRect(), 0},
               {3, QStringLiteral("firefox"), QStringLiteral("Mail"), QRect(), 0}};
    FakeClient client;
    auto control = grant(client);

    // Only handles this control listed name a window.
    control->activateWindow(1);
    EXPECT_EQ(std::vector<Rejection> {Rejection::NoWindow}, client.rejections);

    control->listWindows();
    control->activateWindow(2);
    EXPECT_EQ(std::vector<quint64> {3}, activations);

    windows.pop_back();
    control->activateWindow(2);
    control->activateWindow(99);
    EXPECT_EQ((std::vector<Rejection> {Rejection::NoWindow, Rejection::NoWindow,
                                       Rejection::NoWindow}),
              client.rejections);

    // Activation changes what the user's keys reach, so it waits for them too.
    input.onPointerMotion(clock, QPointF(1, 0));
    control->activateWindow(1);
    EXPECT_EQ(Rejection::Paused, client.rejections.back());
    EXPECT_EQ(std::vector<quint64> {3}, activations);
}

TEST_F(ComputerControlTest, TheClipboardIsSetOnlyWhileGrantedAndNotPaused)
{
    FakeClient client;
    auto control = controller->request(&client, &client, QStringLiteral("Agent"));
    control->setClipboard("pending");
    EXPECT_EQ(std::vector<Rejection> {Rejection::NotGranted}, client.rejections);

    answer(ConsentResult::Allowed);
    control->setClipboard("line one\nline two");
    EXPECT_EQ(std::vector<std::string> {"line one\nline two"}, clipboards);

    // It replaces what the user copied, so it waits for the user too.
    input.onPointerMotion(clock, QPointF(1, 0));
    control->setClipboard("while typing");
    EXPECT_EQ(Rejection::Paused, client.rejections.back());
    EXPECT_EQ(1u, clipboards.size());
}

TEST_F(ComputerControlTest, AWindowIsClosedAndPlacedByItsHandle)
{
    windows = {{7, QStringLiteral("gedit"), QStringLiteral("Notes"), QRect(), 0},
               {3, QStringLiteral("polkit"), QStringLiteral("Authenticate"), QRect(), 0}};
    protectedWindow = 3;
    FakeClient client;
    auto control = grant(client);

    control->closeWindow(1);
    EXPECT_EQ(std::vector<Rejection> {Rejection::NoWindow}, client.rejections);

    control->listWindows();
    control->closeWindow(1);
    control->setWindowFrame(1, QRect(10, 20, 300, 200));
    EXPECT_EQ(std::vector<quint64> {7}, closes);
    using Frames = std::vector<std::pair<quint64, QRect>>;
    EXPECT_EQ((Frames {{7, QRect(10, 20, 300, 200)}}), frames);

    // The dialogs that answer for the user are left alone, though bringing
    // one forward is harmless.
    control->closeWindow(2);
    control->setWindowFrame(2, QRect(0, 0, 100, 100));
    EXPECT_EQ((std::vector<Rejection> {Rejection::NoWindow, Rejection::Protected,
                                       Rejection::Protected}),
              client.rejections);
    control->activateWindow(2);
    EXPECT_EQ(std::vector<quint64> {3}, activations);
    EXPECT_EQ(3u, client.rejections.size());

    windows.erase(windows.begin());
    control->closeWindow(1);
    control->setWindowFrame(1, QRect(0, 0, 100, 100));
    EXPECT_EQ(Rejection::NoWindow, client.rejections[3]);
    EXPECT_EQ(Rejection::NoWindow, client.rejections[4]);

    input.onPointerMotion(clock, QPointF(1, 0));
    control->closeWindow(2);
    EXPECT_EQ(Rejection::Paused, client.rejections.back());
    EXPECT_EQ(std::vector<quint64> {7}, closes);
}
