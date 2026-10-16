// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_SHELL_TEXT_INPUT_H_
#define CUTEFISH_SHELL_SHELL_TEXT_INPUT_H_

#include "ui/event.h"
#include "wayland/protocols/text_input.h"

namespace cutefish::shell {

class Shell;
class ShellWindow;

// The shell's text fields on the input-method relay: one more adapter next
// to text-input-v3 and v1, as exo's TextInput and Ash's views share one
// ui::InputMethod. The keyboard-focused shell window's focused text view
// enables it; the input method's edits go back to that window.
class ShellTextInput final : public wayland::TextInput
{
public:
    ShellTextInput(Shell *shell, wayland::InputMethodRelay *relay);
    ~ShellTextInput() override;

    // The toolkit's textInputChanged(); null disables.
    void update(ShellWindow *window, const ui::TextInputState *state, bool fromInputMethod);
    ShellWindow *window() const { return m_window; }
    bool isEnabled() const { return m_window != nullptr; }

    void windowDestroying(ShellWindow *window);

    // wayland::TextInput:
    bool attached() const override { return m_window != nullptr; }
    bool ownsSurface(const compositor::Surface *surface) const override;
    void sendEdit(const wayland::TextInputEdit &edit) override;
    void detached() override;
    void focusChanged(compositor::Surface *gained, compositor::Surface *lost) override;

private:
    Shell *m_shell;
    ShellWindow *m_window = nullptr;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SHELL_TEXT_INPUT_H_
