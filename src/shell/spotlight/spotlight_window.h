// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_WINDOW_H_
#define CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_WINDOW_H_

#include <memory>
#include <vector>

#include "base/signal.h"
#include "shell/spotlight/spotlight.h"
#include "shell/spotlight/spotlight_view.h"

namespace cutefish::shell {

class Shell;
class ShellWindow;

class SpotlightWindow
{
public:
    SpotlightWindow(Shell *shell, std::unique_ptr<Spotlight> spotlight);
    ~SpotlightWindow();

    Spotlight *spotlight() const { return m_spotlight.get(); }

private:
    void updateGeometry();
    void showWindow();
    void hideWindow();
    void activeChanged();

    Shell *m_shell = nullptr;
    std::unique_ptr<Spotlight> m_spotlight;
    std::unique_ptr<ShellWindow> m_window;
    SpotlightView *m_view = nullptr;
    std::vector<base::Subscription> m_subscriptions;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_WINDOW_H_
