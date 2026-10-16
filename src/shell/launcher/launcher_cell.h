// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_LAUNCHER_LAUNCHER_CELL_H_
#define CUTEFISH_SHELL_LAUNCHER_LAUNCHER_CELL_H_

#include <string>

#include "shell/launcher/launcher_metrics.h"
#include "shell/launcher/launcher_model.h"
#include "shell/launcher/launcher_tween.h"
#include "ui/view.h"

namespace cutefish::ui {
class Label;
class ThemeIcon;
} // namespace cutefish::ui

namespace cutefish::shell {

class LauncherView;

// One application in the launcher's grid: its icon,
// its name in up to two lines, the new mark, the keyboard selection and the
// pressed silhouette. Only the icon takes the pointer; the rest of the cell
// is the page behind it.
class LauncherCell final : public ui::View
{
public:
    LauncherCell(LauncherView *view, std::string id);
    ~LauncherCell() override;

    const std::string &id() const { return m_id; }

    void setItem(const LauncherItem &item);
    const std::string &iconName() const { return m_iconName; }
    void setMetrics(const LauncherMetrics &metrics);
    // The page the cell is on and its index there.
    void setSlot(int page, int index) { m_page = page; m_index = index; }
    int page() const { return m_page; }
    int index() const { return m_index; }

    void setSelected(bool selected);
    bool isSelected() const { return m_selected; }
    // Its icon and name hide while it is dragged.
    void setDragging(bool dragging);
    bool isDragging() const { return m_dragging; }
    void setMenuOpen(bool open);
    bool isPressDimmed() const { return (m_pressed || m_menuOpen) && !m_dragging; }
    // The colour the selection is inked against.
    void setBackdrop(ui::Color color);

    // Its place in the page; animated, as the grid's displaced transitions,
    // when `animate`.
    void moveTo(const ui::PointF &position, bool animate);
    bool isMoving() const { return m_moveX.isRunning() || m_moveY.isRunning(); }
    void finishMove();

    // In the cell's coordinates.
    ui::RectF iconRect() const;
    // The selection plate, in the cell's coordinates; empty while hidden.
    ui::RectF selectionRect() const;
    // The new mark, in the cell's coordinates; empty while hidden.
    ui::RectF newMarkRect() const;

    // Called by the icon; not part of the public API.
    void iconPressed(const ui::PointerEvent &event);
    void iconMoved(const ui::PointerEvent &event);
    void iconReleased(const ui::PointerEvent &event);

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;

private:
    class Icon;

    void updateMask();

    LauncherView *m_view;
    std::string m_id;
    std::string m_iconName;
    bool m_newInstalled = false;
    LauncherMetrics m_metrics;
    int m_page = 0;
    int m_index = 0;
    Icon *m_icon = nullptr;
    ui::ThemeIcon *m_mask = nullptr;
    ui::Label *m_label = nullptr;
    ui::Color m_backdrop {0, 0, 0, 255};
    bool m_selected = false;
    bool m_dragging = false;
    bool m_menuOpen = false;
    bool m_pressed = false;
    // One drag per press.
    bool m_dragArmed = false;
    ui::PointF m_pressPoint;
    LauncherTween m_moveX;
    LauncherTween m_moveY;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_LAUNCHER_LAUNCHER_CELL_H_
