// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "input/key_translator.h"
#include "input/xkb_tracker.h"

using namespace cutefish::input;

namespace {

Timestamp at(int ms)
{
    return Timestamp(std::chrono::milliseconds(ms));
}

struct Delivered {
    uint32_t code;
    bool pressed;

    bool operator==(const Delivered &) const = default;
};

std::ostream &operator<<(std::ostream &stream, const Delivered &key)
{
    return stream << key.code << (key.pressed ? " down" : " up");
}

Delivered down(uint32_t code)
{
    return {code, true};
}

Delivered up(uint32_t code)
{
    return {code, false};
}

class KeyTranslatorTest : public testing::Test
{
protected:
    XkbTracker keymap;
    std::vector<Delivered> delivered;
    KeyTargetFlags target = NoKeyTarget;
    bool passthrough = false;
    bool buttonsHeld = false;
    KeyTranslator translator {&keymap, [this](Timestamp, uint32_t code, bool pressed) {
                                  delivered.push_back({code, pressed});
                              }};
    int time = 0;

    void SetUp() override
    {
        translator.setConvention(ShortcutConvention::Command);
        translator.setTargetProvider([this] {
            return target;
        });
        translator.setPassthroughProvider([this] {
            return passthrough;
        });
        translator.setPointerButtonsProvider([this] {
            return buttonsHeld;
        });
    }

    void press(uint32_t code) { translator.onKey(at(++time), code, true); }

    void release(uint32_t code) { translator.onKey(at(++time), code, false); }

    void tap(uint32_t code)
    {
        press(code);
        release(code);
    }

    std::vector<Delivered> take() { return std::exchange(delivered, {}); }
};

} // namespace

TEST_F(KeyTranslatorTest, ThePcConventionPassesEveryKeyThroughAsItIs)
{
    translator.setConvention(ShortcutConvention::Pc);
    press(KEY_LEFTALT);
    tap(KEY_TAB);
    release(KEY_LEFTALT);
    tap(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTALT), down(KEY_TAB), up(KEY_TAB),
                                      up(KEY_LEFTALT), down(KEY_LEFTMETA), up(KEY_LEFTMETA)}),
              take());
}

TEST_F(KeyTranslatorTest, CommandAloneReachesNoClient)
{
    tap(KEY_LEFTMETA);
    tap(KEY_LEFTALT);
    tap(KEY_LEFTCTRL);
    EXPECT_TRUE(take().empty());
}

TEST_F(KeyTranslatorTest, CommandGoesOutAsCtrlWithTheKeyThatNeedsIt)
{
    press(KEY_LEFTMETA);
    EXPECT_TRUE(take().empty());
    tap(KEY_C);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>(
                      {down(KEY_LEFTCTRL), down(KEY_C), up(KEY_C), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, ShiftGoesOutAtOnce)
{
    press(KEY_LEFTSHIFT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTSHIFT)}), take());
    press(KEY_LEFTMETA);
    tap(KEY_Z);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_Z), up(KEY_Z)}), take());
}

TEST_F(KeyTranslatorTest, CommandLeftMovesToTheLineStartWithoutCtrl)
{
    press(KEY_LEFTMETA);
    tap(KEY_LEFT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_HOME), up(KEY_HOME)}), take());

    press(KEY_LEFTSHIFT);
    tap(KEY_LEFT);
    release(KEY_LEFTSHIFT);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTSHIFT), down(KEY_HOME), up(KEY_HOME),
                                      up(KEY_LEFTSHIFT)}),
              take());
}

TEST_F(KeyTranslatorTest, ARuleLiftsACtrlTheClientAlreadyHas)
{
    press(KEY_LEFTMETA);
    tap(KEY_C);
    take();
    tap(KEY_RIGHT);
    // The Ctrl stays lifted until a key needs it again.
    EXPECT_EQ(std::vector<Delivered>({up(KEY_LEFTCTRL), down(KEY_END), up(KEY_END)}), take());
    tap(KEY_V);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_V), up(KEY_V)}), take());
}

TEST_F(KeyTranslatorTest, AKeyReleasesWhatItWasSentAs)
{
    press(KEY_LEFTMETA);
    press(KEY_LEFT);
    release(KEY_LEFTMETA);
    release(KEY_LEFT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_HOME), up(KEY_HOME)}), take());

    // The convention changing in between does not strand a key either.
    press(KEY_LEFTMETA);
    press(KEY_A);
    translator.setConvention(ShortcutConvention::Pc);
    release(KEY_A);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>(
                      {down(KEY_LEFTCTRL), down(KEY_A), up(KEY_A), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, OptionMovesByWordAndDeletesWords)
{
    press(KEY_LEFTALT);
    tap(KEY_LEFT);
    tap(KEY_BACKSPACE);
    release(KEY_LEFTALT);
    // Option itself never goes out; each rule sends its own Ctrl.
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_LEFT), up(KEY_LEFT),
                                      up(KEY_LEFTCTRL), down(KEY_LEFTCTRL), down(KEY_BACKSPACE),
                                      up(KEY_BACKSPACE), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, CommandBackspaceDeletesToTheLineStart)
{
    press(KEY_LEFTMETA);
    tap(KEY_BACKSPACE);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTSHIFT), down(KEY_HOME), up(KEY_HOME),
                                      up(KEY_LEFTSHIFT), down(KEY_BACKSPACE),
                                      up(KEY_BACKSPACE)}),
              take());
}

TEST_F(KeyTranslatorTest, ControlKeepsItsLineEditingKeysInTextAndCtrlElsewhere)
{
    press(KEY_LEFTCTRL);
    tap(KEY_A);
    tap(KEY_SPACE);
    release(KEY_LEFTCTRL);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_HOME), up(KEY_HOME), down(KEY_LEFTCTRL),
                                      down(KEY_SPACE), up(KEY_SPACE), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, TerminalsCopyWithCtrlShiftAndKeepControlForThePrograms)
{
    target = TerminalTarget;
    press(KEY_LEFTMETA);
    tap(KEY_C);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_LEFTSHIFT), down(KEY_C),
                                      up(KEY_C), up(KEY_LEFTSHIFT), up(KEY_LEFTCTRL)}),
              take());

    press(KEY_LEFTCTRL);
    tap(KEY_C);
    tap(KEY_A);
    release(KEY_LEFTCTRL);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_C), up(KEY_C), down(KEY_A),
                                      up(KEY_A), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, CommandKClearsWithCtrlShiftKOnlyInCutefishTerminal)
{
    target = TerminalTarget | CutefishTerminalTarget;
    press(KEY_LEFTMETA);
    tap(KEY_K);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_LEFTSHIFT), down(KEY_K),
                                      up(KEY_K), up(KEY_LEFTSHIFT), up(KEY_LEFTCTRL)}),
              take());

    target = TerminalTarget;
    press(KEY_LEFTMETA);
    tap(KEY_K);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_L), up(KEY_L),
                                      up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, TerminalsDeleteAWordForwardWithEscapeD)
{
    target = TerminalTarget;
    press(KEY_LEFTALT);
    tap(KEY_DELETE);
    release(KEY_LEFTALT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_ESC), up(KEY_ESC), down(KEY_D), up(KEY_D)}),
              take());
}

TEST_F(KeyTranslatorTest, TerminalsGetNoCommandKeyWithoutARule)
{
    // As Ctrl these would end the shell, suspend the program and run the line.
    target = TerminalTarget;
    press(KEY_LEFTMETA);
    tap(KEY_D);
    tap(KEY_Z);
    tap(KEY_ENTER);
    tap(KEY_UP);
    release(KEY_LEFTMETA);
    EXPECT_TRUE(take().empty());

    press(KEY_LEFTSHIFT);
    press(KEY_LEFTMETA);
    tap(KEY_D);
    release(KEY_LEFTMETA);
    release(KEY_LEFTSHIFT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTSHIFT), up(KEY_LEFTSHIFT)}), take());

    // Other applications still get Command as Ctrl.
    target = NoKeyTarget;
    press(KEY_LEFTMETA);
    tap(KEY_D);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>(
                      {down(KEY_LEFTCTRL), down(KEY_D), up(KEY_D), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, TerminalsSelectAllZoomAndScrollWithCommand)
{
    target = TerminalTarget;
    press(KEY_LEFTMETA);
    tap(KEY_A);
    tap(KEY_EQUAL);
    tap(KEY_PAGEUP);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_LEFTSHIFT), down(KEY_A),
                                      up(KEY_A), up(KEY_LEFTSHIFT), down(KEY_EQUAL),
                                      up(KEY_EQUAL), up(KEY_LEFTCTRL), down(KEY_LEFTSHIFT),
                                      down(KEY_PAGEUP), up(KEY_PAGEUP), up(KEY_LEFTSHIFT)}),
              take());
}

TEST_F(KeyTranslatorTest, CommandDigitPicksATab)
{
    // Terminals and non-Chromium browsers pick tabs with Alt+digit.
    for (const KeyTargetFlags app : {KeyTargetFlags(TerminalTarget),
                                     KeyTargetFlags(BrowserTarget)}) {
        target = app;
        press(KEY_LEFTMETA);
        tap(KEY_3);
        tap(KEY_9);
        release(KEY_LEFTMETA);
        EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTALT), down(KEY_3), up(KEY_3),
                                          up(KEY_LEFTALT), down(KEY_LEFTALT), down(KEY_9),
                                          up(KEY_9), up(KEY_LEFTALT)}),
                  take());
    }

    // Chromium takes Ctrl+digit, and Command+0 stays Ctrl+0 everywhere.
    target = BrowserTarget | ChromiumTarget;
    press(KEY_LEFTMETA);
    tap(KEY_1);
    release(KEY_LEFTMETA);
    target = TerminalTarget;
    press(KEY_LEFTMETA);
    tap(KEY_0);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_1), up(KEY_1),
                                      up(KEY_LEFTCTRL), down(KEY_LEFTCTRL), down(KEY_0),
                                      up(KEY_0), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, BrowsersGoBackWithCommandBracket)
{
    target = BrowserTarget;
    press(KEY_LEFTMETA);
    tap(KEY_LEFTBRACE);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTALT), down(KEY_LEFT), up(KEY_LEFT),
                                      up(KEY_LEFTALT)}),
              take());

    // Elsewhere Command+[ stays Ctrl+[, which editors outdent with.
    target = NoKeyTarget;
    press(KEY_LEFTMETA);
    tap(KEY_LEFTBRACE);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_LEFTBRACE),
                                      up(KEY_LEFTBRACE), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, CommandAndControlShareOneCtrl)
{
    press(KEY_LEFTMETA);
    press(KEY_LEFTCTRL);
    tap(KEY_X);
    release(KEY_LEFTCTRL);
    tap(KEY_X);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL), down(KEY_X), up(KEY_X), down(KEY_X),
                                      up(KEY_X), up(KEY_LEFTCTRL)}),
              take());
}

TEST_F(KeyTranslatorTest, PassthroughSendsCommandAsMetaAtOnce)
{
    passthrough = true;
    press(KEY_LEFTMETA);
    tap(KEY_C);
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>(
                      {down(KEY_LEFTMETA), down(KEY_C), up(KEY_C), up(KEY_LEFTMETA)}),
              take());
}

TEST_F(KeyTranslatorTest, AClickTakesTheHeldModifiers)
{
    press(KEY_LEFTMETA);
    translator.flushModifiers(at(++time));
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTCTRL)}), take());
    release(KEY_LEFTMETA);
    EXPECT_EQ(std::vector<Delivered>({up(KEY_LEFTCTRL)}), take());

    // During a drag a modifier goes out as it goes down.
    buttonsHeld = true;
    press(KEY_LEFTALT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_LEFTALT)}), take());
}

TEST_F(KeyTranslatorTest, ControlClickIsASecondaryClickWithoutCtrl)
{
    press(KEY_LEFTCTRL);
    EXPECT_EQ(uint32_t(BTN_RIGHT), translator.translateButton(BTN_LEFT, true));
    translator.flushModifiers(at(++time));
    EXPECT_TRUE(take().empty());
    release(KEY_LEFTCTRL);
    EXPECT_EQ(uint32_t(BTN_RIGHT), translator.translateButton(BTN_LEFT, false));

    press(KEY_LEFTMETA);
    EXPECT_EQ(uint32_t(BTN_LEFT), translator.translateButton(BTN_LEFT, true));
    EXPECT_EQ(uint32_t(BTN_LEFT), translator.translateButton(BTN_LEFT, false));
}

TEST_F(KeyTranslatorTest, AltGrStaysACharacterModifier)
{
    ASSERT_TRUE(keymap.updateKeyboardLayout("de"));
    press(KEY_RIGHTALT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_RIGHTALT)}), take());
    tap(KEY_Q);
    release(KEY_RIGHTALT);
    EXPECT_EQ(std::vector<Delivered>({down(KEY_Q), up(KEY_Q), up(KEY_RIGHTALT)}), take());
}

TEST_F(KeyTranslatorTest, DeliveredKeysIncludeTheSentModifiers)
{
    press(KEY_LEFTMETA);
    press(KEY_C);
    std::vector<uint32_t> keys = translator.deliveredKeys();
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(std::vector<uint32_t>({KEY_LEFTCTRL, KEY_C}), keys);
}

TEST(KeyTargets, KnownApplicationsAreClassified)
{
    EXPECT_EQ(KeyTargetFlags(TerminalTarget | CutefishTerminalTarget),
              classifyApplication(QStringLiteral("cutefish-terminal")));
    EXPECT_EQ(KeyTargetFlags(TerminalTarget),
              classifyApplication(QStringLiteral("org.gnome.Console")));
    EXPECT_EQ(KeyTargetFlags(BrowserTarget | ChromiumTarget),
              classifyApplication(QStringLiteral("google-chrome")));
    EXPECT_EQ(KeyTargetFlags(BrowserTarget), classifyApplication(QStringLiteral("firefox")));
    EXPECT_EQ(KeyTargetFlags(FileManagerTarget),
              classifyApplication(QStringLiteral("org.gnome.Nautilus")));
    EXPECT_EQ(KeyTargetFlags(NoKeyTarget), classifyApplication(QStringLiteral("code")));
}

TEST(XkbTracker, FindsTheKeyForAKeysym)
{
    XkbTracker tracker;
    EXPECT_EQ(uint32_t(KEY_B), tracker.evdevCodeForKeysym(XKB_KEY_b));
    ASSERT_TRUE(tracker.updateKeyboardLayout("us-dvorak"));
    EXPECT_EQ(uint32_t(KEY_N), tracker.evdevCodeForKeysym(XKB_KEY_b));
}
