// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_LAUNCHER_LAUNCHER_WINDOW_H_
#define CUTEFISH_SHELL_LAUNCHER_LAUNCHER_WINDOW_H_

#include <memory>
#include <string>
#include <vector>

#include "base/signal.h"
#include "shell/shell.h"

namespace cutefish::shell {

class Dock;
class Launcher;
class LauncherView;
class LauncherWallpaper;
class Wallpaper;
class ShellWindow;

// The launcher window: a window of
// the Launcher role covering the primary output, status bar included, with
// room left for the dock; shown, activated and hidden by the Launcher it is
// given, and hidden again when it loses the activation to anything but its
// own menu, a drag or the capture tools. The compositor animates its
// opening and closing by its role.
class LauncherWindow : public Component
{
public:
    // `dock` gives the room to leave; it may be null.
    // `wallpaper` is the desktop's, whose decode the launcher shrinks.
    LauncherWindow(Shell *shell, std::unique_ptr<Launcher> launcher, Dock *dock, Wallpaper *wallpaper);
    ~LauncherWindow() override;

    Launcher &launcher() { return *m_launcher; }
    ShellWindow *window() const { return m_window.get(); }
    LauncherView *view() const { return m_view; }
    LauncherWallpaper &wallpaper() { return *m_wallpaper; }

private:
    void showWindow();
    void hideWindow();
    void updateGeometry();
    void activeChanged();
    bool keepsShownWhileInactive() const;

    Shell *m_shell;
    std::unique_ptr<Launcher> m_launcher;
    Dock *m_dock;
    std::unique_ptr<LauncherWallpaper> m_wallpaper;
    std::unique_ptr<ShellWindow> m_window;
    LauncherView *m_view = nullptr;
    std::vector<base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_LAUNCHER_LAUNCHER_WINDOW_H_
