// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_TILING_WINDOW_TILE_POPOVER_H_
#define CUTEFISH_SHELL_TILING_WINDOW_TILE_POPOVER_H_

#include <memory>
#include "base/geometry.h"
#include "base/timer.h"
#include "shell/shell.h"

namespace cutefish::wm {
class Window;
}

namespace cutefish::shell {

class ShellWindow;
enum class TileAction;

class WindowTilePopover : public Component
{
public:
    explicit WindowTilePopover(Shell *shell);
    ~WindowTilePopover() override;

    void show(wm::Window *window, const base::PointF &buttonCenter);
    void hide();
    bool isShown() const;
    void executeAction(TileAction action);

    void notifyButtonHover(bool hovered);
    void startDismiss();
    void cancelDismiss();

private:
    class PopoverView;

    Shell *m_shell;
    wm::Window *m_targetWindow = nullptr;
    std::unique_ptr<ShellWindow> m_window;
    PopoverView *m_view = nullptr;
    base::Timer m_dismissTimer;
    bool m_buttonHovered = false;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_TILING_WINDOW_TILE_POPOVER_H_
