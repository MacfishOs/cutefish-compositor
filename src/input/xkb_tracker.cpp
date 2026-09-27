// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/xkb_tracker.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "input/xkb_tracker.h"

#include <cstdlib>
#include <iterator>

#include "core/logging.h"

namespace cutefish::input {

XkbTracker::XkbTracker()
    : m_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS))
{
    updateKeyboardLayoutInternal(nullptr);
}

XkbTracker::~XkbTracker() = default;

bool XkbTracker::updateKeyboardLayout(const std::string &name)
{
    const char *const locks[] = {XKB_MOD_NAME_CAPS, XKB_MOD_NAME_NUM};
    bool locked[std::size(locks)] = {};
    for (size_t i = 0; i < std::size(locks); ++i)
        locked[i] = m_state
                && xkb_state_mod_name_is_active(m_state.get(), locks[i], XKB_STATE_MODS_LOCKED)
                        > 0;

    if (name.empty()) {
        if (!updateKeyboardLayoutInternal(nullptr))
            return false;
    } else {
        // ui::XkbKeyboardLayoutEngine::ParseLayoutName: "layout-variant".
        std::string layout = name;
        std::string variant;
        const size_t dash = name.find('-');
        if (dash != std::string::npos) {
            layout = name.substr(0, dash);
            variant = name.substr(dash + 1);
        }

        xkb_rule_names names = {};
        names.model = "pc101";
        names.layout = layout.c_str();
        names.variant = variant.c_str();
        names.options = "";
        if (!updateKeyboardLayoutInternal(&names))
            return false;
    }
    m_layoutName = name;

    xkb_mod_mask_t lockedMask = 0;
    for (size_t i = 0; i < std::size(locks); ++i) {
        const xkb_mod_index_t index = xkb_keymap_mod_get_index(m_keymap.get(), locks[i]);
        if (locked[i] && index != XKB_MOD_INVALID)
            lockedMask |= 1u << index;
    }
    xkb_state_update_mask(m_state.get(), 0, 0, lockedMask, 0, 0, 0);
    return true;
}

bool XkbTracker::updateKeyboardLayoutInternal(const xkb_rule_names *names)
{
    if (!m_context) {
        qCCritical(cfInput) << "Failed to create an xkb context";
        return false;
    }

    std::unique_ptr<xkb_keymap, KeymapDeleter> keymap(
            xkb_keymap_new_from_names(m_context.get(), names, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!keymap) {
        qCWarning(cfInput) << "Failed to compile the requested xkb keymap";
        return false;
    }

    std::unique_ptr<xkb_state, StateDeleter> state(xkb_state_new(keymap.get()));
    if (!state) {
        qCWarning(cfInput) << "Failed to create an xkb state";
        return false;
    }

    m_keymap = std::move(keymap);
    m_state = std::move(state);
    return true;
}

void XkbTracker::updateKey(uint32_t evdevCode, bool pressed)
{
    if (!m_state)
        return;
    xkb_state_update_key(m_state.get(), evdevCode + kEvdevToXkbOffset,
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

uint32_t XkbTracker::keysym(uint32_t evdevCode) const
{
    if (!m_state)
        return XKB_KEY_NoSymbol;
    return xkb_state_key_get_one_sym(m_state.get(), evdevCode + kEvdevToXkbOffset);
}

uint32_t XkbTracker::baseKeysym(uint32_t evdevCode) const
{
    if (!m_state)
        return XKB_KEY_NoSymbol;
    const xkb_keycode_t keycode = evdevCode + kEvdevToXkbOffset;
    const xkb_layout_index_t layout = xkb_state_key_get_layout(m_state.get(), keycode);
    const xkb_keysym_t *syms = nullptr;
    const int count = xkb_keymap_key_get_syms_by_level(m_keymap.get(), keycode, layout, 0, &syms);
    const xkb_keysym_t keysym =
            count > 0 ? syms[0] : xkb_state_key_get_one_sym(m_state.get(), keycode);
    return xkb_keysym_to_lower(keysym);
}

bool XkbTracker::modifierPressed(const char *name) const
{
    return m_state
            && xkb_state_mod_name_is_active(
                       m_state.get(), name,
                       static_cast<xkb_state_component>(XKB_STATE_MODS_DEPRESSED
                                                        | XKB_STATE_MODS_LATCHED))
            > 0;
}

std::string XkbTracker::keymap() const
{
    if (!m_keymap)
        return std::string();

    char *text = xkb_keymap_get_as_string(m_keymap.get(), XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!text)
        return std::string();

    std::string result(text);
    std::free(text);
    return result;
}

KeyboardModifiers XkbTracker::modifiers() const
{
    if (!m_state)
        return KeyboardModifiers();

    return KeyboardModifiers {
            xkb_state_serialize_mods(m_state.get(), XKB_STATE_MODS_DEPRESSED),
            xkb_state_serialize_mods(m_state.get(), XKB_STATE_MODS_LOCKED),
            xkb_state_serialize_mods(m_state.get(), XKB_STATE_MODS_LATCHED),
            xkb_state_serialize_layout(m_state.get(), XKB_STATE_LAYOUT_EFFECTIVE),
    };
}

bool XkbTracker::keyRepeats(uint32_t evdevCode) const
{
    return m_keymap && xkb_keymap_key_repeats(m_keymap.get(), evdevCode + kEvdevToXkbOffset);
}

uint32_t XkbTracker::leds() const
{
    if (!m_state)
        return 0;
    uint32_t leds = 0;
    for (const auto &[name, led] : {std::pair {XKB_LED_NAME_NUM, KeyboardLed::NumLock},
                                    std::pair {XKB_LED_NAME_CAPS, KeyboardLed::CapsLock},
                                    std::pair {XKB_LED_NAME_SCROLL, KeyboardLed::ScrollLock}}) {
        if (xkb_state_led_name_is_active(m_state.get(), name) > 0)
            leds |= led;
    }
    return leds;
}

bool XkbTracker::modifierActive(const char *name) const
{
    return m_state
            && xkb_state_mod_name_is_active(m_state.get(), name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

} // namespace cutefish::input
