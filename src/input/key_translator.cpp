// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/key_translator.h"

#include <algorithm>
#include <array>

#include <QSet>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "input/xkb_tracker.h"

namespace cutefish::input {
namespace {

// The modifiers held when a rule is looked up.
enum Role : uint32_t {
    Command = 1u << 0,
    Option = 1u << 1,
    Control = 1u << 2,
    Shift = 1u << 3,
};

// The modifiers a rule sends with its key, by kind.
enum ModifierKind : uint32_t {
    NoKind = 0,
    CtrlKind = 1u << 0,
    AltKind = 1u << 1,
    ShiftKind = 1u << 2,
    MetaKind = 1u << 3,
    Level3Kind = 1u << 4,
};

enum class Scope {
    Everywhere,
    NotTerminal,
    Terminal,
    CutefishTerminal,
    Browser,
    Chromium,
    // Browsers not built on Chromium, which pick tabs with Alt+digit.
    OtherBrowser,
    FileManager,
};

// One key a rule sends: an evdev code, the key typing `keysym` in the active
// layout, or, with neither, the key that was pressed.
struct Stroke {
    uint32_t modifiers = 0;
    uint32_t code = 0;
    uint32_t keysym = 0;
};

struct Rule {
    Scope scope;
    uint32_t roles;
    // The key: evdev codes code..lastCode for keys every layout has in the
    // same place, a base keysym otherwise.
    uint32_t code;
    uint32_t lastCode;
    uint32_t keysym;
    // Also matches with Shift held, which is then sent along: Command+Shift+Left
    // selects to the start of the line as Command+Left moves there.
    bool withShift;
    // Every stroke but the last is tapped; the last stays down with the key.
    // No strokes swallows the key.
    int strokeCount;
    std::array<Stroke, 2> strokes;
};

constexpr Stroke send(uint32_t modifiers, uint32_t code)
{
    return {modifiers, code, 0};
}

constexpr Stroke type(uint32_t modifiers, uint32_t keysym)
{
    return {modifiers, 0, keysym};
}

constexpr Stroke same(uint32_t modifiers)
{
    return {modifiers, 0, 0};
}

constexpr Rule key(Scope scope, uint32_t roles, uint32_t code, Stroke stroke,
                   bool withShift = false)
{
    return {scope, roles, code, code, 0, withShift, 1, {stroke, {}}};
}

constexpr Rule key(Scope scope, uint32_t roles, uint32_t code, Stroke first, Stroke second)
{
    return {scope, roles, code, code, 0, false, 2, {first, second}};
}

constexpr Rule keys(Scope scope, uint32_t roles, uint32_t first, uint32_t last, Stroke stroke)
{
    return {scope, roles, first, last, 0, false, 1, {stroke, {}}};
}

constexpr Rule sym(Scope scope, uint32_t roles, uint32_t keysym, Stroke stroke,
                   bool withShift = false)
{
    return {scope, roles, 0, 0, keysym, withShift, 1, {stroke, {}}};
}

constexpr Rule sym(Scope scope, uint32_t roles, uint32_t keysym, Stroke first, Stroke second)
{
    return {scope, roles, 0, 0, keysym, false, 2, {first, second}};
}


constexpr uint32_t None = NoKind;
constexpr uint32_t Ctrl = CtrlKind;
constexpr uint32_t Alt = AltKind;
constexpr uint32_t ShiftMod = ShiftKind;

// Looked up in order; the first match wins, so scoped rules come first. Any
// key no rule matches goes out with Command as Ctrl, except in a terminal,
// where it is dropped (see applyRule). Modelled on Toshy's default keymaps
// (General GUI, General Terminals, browsers, Finder mods).
constexpr auto kRules = std::to_array<Rule>({
        // Terminals take their clipboard and tab shortcuts with Ctrl+Shift and
        // leave Ctrl to the program; line editing speaks readline.
        sym(Scope::Terminal, Command, XKB_KEY_c, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_v, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_t, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_w, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_n, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_f, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_a, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_equal, same(Ctrl), true),
        sym(Scope::Terminal, Command, XKB_KEY_minus, same(Ctrl)),
        key(Scope::Terminal, Command, KEY_0, same(Ctrl)),
        // The scrollback, which terminals page through with Shift.
        key(Scope::Terminal, Command, KEY_HOME, same(ShiftMod)),
        key(Scope::Terminal, Command, KEY_END, same(ShiftMod)),
        key(Scope::Terminal, Command, KEY_PAGEUP, same(ShiftMod)),
        key(Scope::Terminal, Command, KEY_PAGEDOWN, same(ShiftMod)),
        // Clears the scrollback as well; other terminals would take Ctrl+Shift+K as Ctrl+K.
        sym(Scope::CutefishTerminal, Command, XKB_KEY_k, same(Ctrl | ShiftMod)),
        sym(Scope::Terminal, Command, XKB_KEY_k, type(Ctrl, XKB_KEY_l)),
        sym(Scope::Terminal, Command, XKB_KEY_period, type(Ctrl, XKB_KEY_c)),
        key(Scope::Terminal, Command, KEY_LEFT, send(None, KEY_HOME)),
        key(Scope::Terminal, Command, KEY_RIGHT, send(None, KEY_END)),
        key(Scope::Terminal, Command, KEY_BACKSPACE, type(Ctrl, XKB_KEY_u)),
        key(Scope::Terminal, Command, KEY_DELETE, type(Ctrl, XKB_KEY_k)),
        key(Scope::Terminal, Option, KEY_LEFT, type(Alt, XKB_KEY_b)),
        key(Scope::Terminal, Option, KEY_RIGHT, type(Alt, XKB_KEY_f)),
        key(Scope::Terminal, Option, KEY_BACKSPACE, type(Ctrl, XKB_KEY_w)),
        key(Scope::Terminal, Option, KEY_DELETE, send(None, KEY_ESC), type(None, XKB_KEY_d)),
        // Command+1..9 picks a tab: Alt+digit in terminals and non-Chromium
        // browsers; Chromium takes Ctrl+digit as it comes. By position, since
        // some layouts type symbols on the digit row.
        keys(Scope::Terminal, Command, KEY_1, KEY_9, same(Alt)),


        sym(Scope::Chromium, Command, XKB_KEY_y, type(Ctrl, XKB_KEY_h)),
        keys(Scope::OtherBrowser, Command, KEY_1, KEY_9, same(Alt)),
        sym(Scope::Browser, Command, XKB_KEY_bracketleft, send(Alt, KEY_LEFT)),
        sym(Scope::Browser, Command, XKB_KEY_bracketright, send(Alt, KEY_RIGHT)),
        key(Scope::Browser, Command | Option, KEY_LEFT, send(Ctrl, KEY_PAGEUP)),
        key(Scope::Browser, Command | Option, KEY_RIGHT, send(Ctrl, KEY_PAGEDOWN)),
        sym(Scope::Browser, Command | Option, XKB_KEY_i, same(Ctrl | ShiftMod)),
        sym(Scope::Browser, Command | Option, XKB_KEY_j, same(Ctrl | ShiftMod)),
        sym(Scope::Browser, Command | Option, XKB_KEY_u, type(Ctrl, XKB_KEY_u)),

        sym(Scope::FileManager, Command, XKB_KEY_bracketleft, send(Alt, KEY_LEFT)),
        sym(Scope::FileManager, Command, XKB_KEY_bracketright, send(Alt, KEY_RIGHT)),
        key(Scope::FileManager, Command, KEY_UP, send(Alt, KEY_UP)),
        key(Scope::FileManager, Command, KEY_DOWN, send(None, KEY_ENTER)),
        key(Scope::FileManager, Command, KEY_BACKSPACE, send(None, KEY_DELETE)),
        sym(Scope::FileManager, Command, XKB_KEY_i, send(Alt, KEY_ENTER)),
        sym(Scope::FileManager, Command | Shift, XKB_KEY_period, type(Ctrl, XKB_KEY_h)),

        sym(Scope::Everywhere, Command | Shift, XKB_KEY_bracketleft, send(Ctrl, KEY_PAGEUP)),
        sym(Scope::Everywhere, Command | Shift, XKB_KEY_bracketright, send(Ctrl, KEY_PAGEDOWN)),

        // Text navigation.
        key(Scope::NotTerminal, Command, KEY_LEFT, send(None, KEY_HOME), true),
        key(Scope::NotTerminal, Command, KEY_RIGHT, send(None, KEY_END), true),
        key(Scope::NotTerminal, Command, KEY_UP, send(Ctrl, KEY_HOME), true),
        key(Scope::NotTerminal, Command, KEY_DOWN, send(Ctrl, KEY_END), true),
        key(Scope::NotTerminal, Option, KEY_LEFT, send(Ctrl, KEY_LEFT), true),
        key(Scope::NotTerminal, Option, KEY_RIGHT, send(Ctrl, KEY_RIGHT), true),
        key(Scope::NotTerminal, Option, KEY_BACKSPACE, send(Ctrl, KEY_BACKSPACE)),
        key(Scope::NotTerminal, Option, KEY_DELETE, send(Ctrl, KEY_DELETE)),
        key(Scope::NotTerminal, Command, KEY_BACKSPACE, send(ShiftMod, KEY_HOME),
            send(None, KEY_BACKSPACE)),
        key(Scope::NotTerminal, Command, KEY_DELETE, send(ShiftMod, KEY_END),
            send(None, KEY_DELETE)),
        sym(Scope::NotTerminal, Command, XKB_KEY_period, send(None, KEY_ESC)),

        // Control's line editing keys, as text fields take them there.
        sym(Scope::NotTerminal, Control, XKB_KEY_a, send(None, KEY_HOME), true),
        sym(Scope::NotTerminal, Control, XKB_KEY_e, send(None, KEY_END), true),
        sym(Scope::NotTerminal, Control, XKB_KEY_b, send(None, KEY_LEFT), true),
        sym(Scope::NotTerminal, Control, XKB_KEY_f, send(None, KEY_RIGHT), true),
        sym(Scope::NotTerminal, Control, XKB_KEY_p, send(None, KEY_UP), true),
        sym(Scope::NotTerminal, Control, XKB_KEY_n, send(None, KEY_DOWN), true),
        sym(Scope::NotTerminal, Control, XKB_KEY_d, send(None, KEY_DELETE)),
        sym(Scope::NotTerminal, Control, XKB_KEY_h, send(None, KEY_BACKSPACE)),
        sym(Scope::NotTerminal, Control, XKB_KEY_k, send(ShiftMod, KEY_END),
            send(None, KEY_DELETE)),
});

bool inScope(Scope scope, KeyTargetFlags target)
{
    switch (scope) {
    case Scope::Everywhere:
        return true;
    case Scope::NotTerminal:
        return !(target & TerminalTarget);
    case Scope::Terminal:
        return target & TerminalTarget;
    case Scope::CutefishTerminal:
        return target & CutefishTerminalTarget;
    case Scope::Browser:
        return target & BrowserTarget;
    case Scope::Chromium:
        return target & ChromiumTarget;
    case Scope::OtherBrowser:
        return (target & BrowserTarget) && !(target & ChromiumTarget);
    case Scope::FileManager:
        return target & FileManagerTarget;
    }
    return false;
}

uint32_t defaultCode(ModifierKind kind)
{
    switch (kind) {
    case CtrlKind:
        return KEY_LEFTCTRL;
    case AltKind:
        return KEY_LEFTALT;
    case ShiftKind:
        return KEY_LEFTSHIFT;
    default:
        return 0;
    }
}

} // namespace

KeyTargetFlags classifyApplication(const QString &applicationId)
{
    // Lower-cased application ids and X11 classes, after Toshy's lists.
    static const QSet<QString> terminals = {
            QStringLiteral("alacritty"),
            QStringLiteral("kitty"),
            QStringLiteral("foot"),
            QStringLiteral("footclient"),
            QStringLiteral("org.gnome.terminal"),
            QStringLiteral("gnome-terminal"),
            QStringLiteral("gnome-terminal-server"),
            QStringLiteral("org.gnome.console"),
            QStringLiteral("kgx"),
            QStringLiteral("org.gnome.ptyxis"),
            QStringLiteral("org.kde.konsole"),
            QStringLiteral("konsole"),
            QStringLiteral("org.kde.yakuake"),
            QStringLiteral("yakuake"),
            QStringLiteral("xfce4-terminal"),
            QStringLiteral("terminator"),
            QStringLiteral("tilix"),
            QStringLiteral("com.gexperts.tilix"),
            QStringLiteral("org.wezfurlong.wezterm"),
            QStringLiteral("wezterm"),
            QStringLiteral("wezterm-gui"),
            QStringLiteral("com.mitchellh.ghostty"),
            QStringLiteral("ghostty"),
            QStringLiteral("io.elementary.terminal"),
            QStringLiteral("com.system76.cosmicterm"),
            QStringLiteral("com.raggesilver.blackbox"),
            QStringLiteral("deepin-terminal"),
            QStringLiteral("dev.warp.warp"),
            QStringLiteral("contour"),
            QStringLiteral("qterminal"),
            QStringLiteral("lxterminal"),
            QStringLiteral("mate-terminal"),
            QStringLiteral("terminology"),
            QStringLiteral("guake"),
            QStringLiteral("tilda"),
            QStringLiteral("sakura"),
            QStringLiteral("roxterm"),
            QStringLiteral("tabby"),
            QStringLiteral("hyper"),
            QStringLiteral("xterm"),
            QStringLiteral("uxterm"),
            QStringLiteral("urxvt"),
            QStringLiteral("st"),
            QStringLiteral("st-256color"),
    };
    static const QSet<QString> chromium = {
            QStringLiteral("chromium"),
            QStringLiteral("chromium-browser"),
            QStringLiteral("org.chromium.chromium"),
            QStringLiteral("google-chrome"),
            QStringLiteral("google-chrome-stable"),
            QStringLiteral("google-chrome-beta"),
            QStringLiteral("google-chrome-unstable"),
            QStringLiteral("com.google.chrome"),
            QStringLiteral("brave-browser"),
            QStringLiteral("com.brave.browser"),
            QStringLiteral("microsoft-edge"),
            QStringLiteral("microsoft-edge-stable"),
            QStringLiteral("com.microsoft.edge"),
            QStringLiteral("vivaldi"),
            QStringLiteral("vivaldi-stable"),
            QStringLiteral("opera"),
            QStringLiteral("thorium-browser"),
    };
    static const QSet<QString> browsers = {
            QStringLiteral("firefox"),
            QStringLiteral("firefox-esr"),
            QStringLiteral("org.mozilla.firefox"),
            QStringLiteral("librewolf"),
            QStringLiteral("io.gitlab.librewolf-community"),
            QStringLiteral("floorp"),
            QStringLiteral("one.ablaze.floorp"),
            QStringLiteral("waterfox"),
            QStringLiteral("zen"),
            QStringLiteral("app.zen_browser.zen"),
            QStringLiteral("org.gnome.epiphany"),
            QStringLiteral("org.kde.falkon"),
            QStringLiteral("falkon"),
    };
    static const QSet<QString> fileManagers = {
            QStringLiteral("cutefish-filemanager"),
            QStringLiteral("org.gnome.nautilus"),
            QStringLiteral("nautilus"),
            QStringLiteral("org.kde.dolphin"),
            QStringLiteral("dolphin"),
            QStringLiteral("thunar"),
            QStringLiteral("nemo"),
            QStringLiteral("caja"),
            QStringLiteral("pcmanfm"),
            QStringLiteral("pcmanfm-qt"),
            QStringLiteral("io.elementary.files"),
            QStringLiteral("com.system76.cosmicfiles"),
            QStringLiteral("org.kde.krusader"),
            QStringLiteral("peony"),
            QStringLiteral("dde-file-manager"),
    };

    const QString id = applicationId.toLower();
    if (id == QLatin1String("cutefish-terminal"))
        return TerminalTarget | CutefishTerminalTarget;
    if (terminals.contains(id))
        return TerminalTarget;
    if (chromium.contains(id))
        return BrowserTarget | ChromiumTarget;
    if (browsers.contains(id))
        return BrowserTarget;
    if (fileManagers.contains(id))
        return FileManagerTarget;
    return NoKeyTarget;
}

KeyTranslator::KeyTranslator(const XkbTracker *keymap, DeliverKey deliver)
    : m_keymap(keymap)
    , m_deliver(std::move(deliver))
{
}

bool KeyTranslator::passthrough() const
{
    return m_convention == ShortcutConvention::Pc || (m_passthrough && m_passthrough());
}

bool KeyTranslator::isModifier(uint32_t code) const
{
    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return true;
    default:
        return false;
    }
}

// Command, Option and Control wait for a key; Shift and AltGr pick the
// characters typed, so clients need them at once.
bool KeyTranslator::isHeldBack(uint32_t code) const
{
    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return true;
    case KEY_RIGHTALT:
        return m_keymap->baseKeysym(KEY_RIGHTALT) != XKB_KEY_ISO_Level3_Shift;
    default:
        return false;
    }
}

uint32_t KeyTranslator::deliveredModifier(uint32_t code, bool passthrough) const
{
    if (passthrough)
        return code;
    if (code == KEY_LEFTMETA)
        return KEY_LEFTCTRL;
    if (code == KEY_RIGHTMETA)
        return KEY_RIGHTCTRL;
    return code;
}

uint32_t KeyTranslator::heldRoles() const
{
    uint32_t roles = 0;
    for (const uint32_t code : m_heldModifiers) {
        switch (code) {
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            roles |= Command;
            break;
        case KEY_LEFTALT:
            roles |= Option;
            break;
        case KEY_RIGHTALT:
            if (isHeldBack(code))
                roles |= Option;
            break;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            roles |= Control;
            break;
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            roles |= Shift;
            break;
        }
    }
    return roles;
}

std::set<uint32_t> KeyTranslator::wantedModifiers(bool passthrough, bool immediateOnly) const
{
    std::set<uint32_t> wanted;
    for (const uint32_t code : m_heldModifiers) {
        if (!immediateOnly || passthrough || !isHeldBack(code))
            wanted.insert(deliveredModifier(code, passthrough));
    }
    return wanted;
}

void KeyTranslator::syncModifiers(Timestamp timestamp, const std::set<uint32_t> &target)
{
    const std::set<uint32_t> delivered = m_deliveredModifiers;
    for (const uint32_t code : delivered) {
        if (!target.contains(code)) {
            m_deliveredModifiers.erase(code);
            m_deliver(timestamp, code, false);
        }
    }
    // In the order the keys went down, then whatever a rule adds.
    std::vector<uint32_t> order;
    const auto add = [&](uint32_t code) {
        if (target.contains(code) && std::find(order.begin(), order.end(), code) == order.end())
            order.push_back(code);
    };
    for (const uint32_t code : m_heldModifiers) {
        add(deliveredModifier(code, false));
        add(code);
    }
    for (const uint32_t code : target)
        add(code);
    for (const uint32_t code : order) {
        if (m_deliveredModifiers.insert(code).second)
            m_deliver(timestamp, code, true);
    }
}

void KeyTranslator::syncModifierKinds(Timestamp timestamp, uint32_t kinds)
{
    const auto kindOf = [this](uint32_t code) -> ModifierKind {
        switch (code) {
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            return CtrlKind;
        case KEY_LEFTALT:
            return AltKind;
        case KEY_RIGHTALT:
            return isHeldBack(code) ? AltKind : Level3Kind;
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            return ShiftKind;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            return MetaKind;
        default:
            return NoKind;
        }
    };

    std::set<uint32_t> target;
    uint32_t covered = NoKind;
    for (const uint32_t code : m_deliveredModifiers) {
        if (kinds & kindOf(code)) {
            target.insert(code);
            covered |= kindOf(code);
        }
    }
    for (const ModifierKind kind : {CtrlKind, AltKind, ShiftKind}) {
        if ((kinds & kind) && !(covered & kind))
            target.insert(defaultCode(kind));
    }
    syncModifiers(timestamp, target);
}

void KeyTranslator::pressKey(Timestamp timestamp, uint32_t code)
{
    if (m_deliveredKeys[code]++ == 0)
        m_deliver(timestamp, code, true);
}

void KeyTranslator::releaseKey(Timestamp timestamp, uint32_t code)
{
    auto it = m_deliveredKeys.find(code);
    if (it == m_deliveredKeys.end())
        return;
    if (--it->second == 0) {
        m_deliveredKeys.erase(it);
        m_deliver(timestamp, code, false);
    }
}

bool KeyTranslator::applyRule(Timestamp timestamp, uint32_t code)
{
    const uint32_t roles = heldRoles();
    const KeyTargetFlags target = m_target ? m_target() : NoKeyTarget;
    const uint32_t keysym = m_keymap->baseKeysym(code);
    const auto match = std::find_if(kRules.begin(), kRules.end(), [&](const Rule &rule) {
        if (rule.code ? code < rule.code || code > rule.lastCode : rule.keysym != keysym)
            return false;
        if (rule.roles != roles && !(rule.withShift && (rule.roles | Shift) == roles))
            return false;
        return inScope(rule.scope, target);
    });
    if (match == kRules.end()) {
        // Ctrl belongs to the program in a terminal, so a Command key as Ctrl
        // would type a control character: Command+D would end the shell.
        if (!(roles & Command) || !(target & TerminalTarget))
            return false;
        m_presses.emplace(code, Press {0, true});
        return true;
    }

    const uint32_t shift = (match->withShift && (roles & Shift)) ? ShiftKind : NoKind;
    Press press {0, true};
    for (int i = 0; i < match->strokeCount; ++i) {
        const Stroke &stroke = match->strokes[i];
        const uint32_t out = stroke.code ? stroke.code
                : stroke.keysym          ? m_keymap->evdevCodeForKeysym(stroke.keysym)
                                         : code;
        if (!out)
            continue;
        syncModifierKinds(timestamp, stroke.modifiers | shift);
        pressKey(timestamp, out);
        if (i + 1 < match->strokeCount)
            releaseKey(timestamp, out);
        else
            press.delivered = out;
    }
    m_presses.emplace(code, press);
    return true;
}

void KeyTranslator::onKey(Timestamp timestamp, uint32_t evdevCode, bool pressed)
{
    const bool passthrough = this->passthrough();

    if (isModifier(evdevCode)) {
        if (pressed) {
            if (std::find(m_heldModifiers.begin(), m_heldModifiers.end(), evdevCode)
                != m_heldModifiers.end())
                return;
            m_heldModifiers.push_back(evdevCode);
            if (passthrough || (m_pointerButtonsHeld && m_pointerButtonsHeld())) {
                syncModifiers(timestamp, wantedModifiers(passthrough, false));
            } else if (!isHeldBack(evdevCode)) {
                std::set<uint32_t> target = m_deliveredModifiers;
                target.insert(deliveredModifier(evdevCode, passthrough));
                syncModifiers(timestamp, target);
            }
            return;
        }
        m_heldModifiers.erase(
                std::remove(m_heldModifiers.begin(), m_heldModifiers.end(), evdevCode),
                m_heldModifiers.end());
        // A key's release takes back what it sent, whatever the mode is now.
        std::set<uint32_t> wanted = wantedModifiers(false, false);
        wanted.merge(wantedModifiers(true, false));
        std::set<uint32_t> target;
        for (const uint32_t code : m_deliveredModifiers) {
            if (wanted.contains(code))
                target.insert(code);
        }
        syncModifiers(timestamp, target);
        return;
    }

    if (pressed) {
        if (m_presses.contains(evdevCode))
            return;
        if (!passthrough && applyRule(timestamp, evdevCode))
            return;
        syncModifiers(timestamp, wantedModifiers(passthrough, false));
        pressKey(timestamp, evdevCode);
        m_presses.emplace(evdevCode, Press {evdevCode, false});
        return;
    }

    const auto it = m_presses.find(evdevCode);
    if (it == m_presses.end())
        return;
    const Press press = it->second;
    m_presses.erase(it);
    if (press.delivered)
        releaseKey(timestamp, press.delivered);
    if (press.rule) {
        // Back to what the held keys send: a modifier only the rule wanted
        // goes, one it lifted stays held back until needed, Shift returns.
        std::set<uint32_t> target = wantedModifiers(passthrough, true);
        for (const uint32_t code : wantedModifiers(passthrough, false)) {
            if (m_deliveredModifiers.contains(code))
                target.insert(code);
        }
        syncModifiers(timestamp, target);
    }
}

void KeyTranslator::flushModifiers(Timestamp timestamp)
{
    // A secondary click made of Control+click carries no Ctrl.
    if (m_secondaryClick)
        return;
    syncModifiers(timestamp, wantedModifiers(passthrough(), false));
}

uint32_t KeyTranslator::translateButton(uint32_t evdevButton, bool pressed)
{
    if (evdevButton != BTN_LEFT)
        return evdevButton;
    if (!pressed) {
        if (!std::exchange(m_secondaryClick, false))
            return evdevButton;
        return BTN_RIGHT;
    }
    if (passthrough() || heldRoles() != Control)
        return evdevButton;
    m_secondaryClick = true;
    return BTN_RIGHT;
}

std::vector<uint32_t> KeyTranslator::deliveredKeys() const
{
    std::vector<uint32_t> keys(m_deliveredModifiers.begin(), m_deliveredModifiers.end());
    for (const auto &[code, count] : m_deliveredKeys)
        keys.push_back(code);
    return keys;
}

} // namespace cutefish::input
