// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SWITCHER_SWITCHER_VIEW_H_
#define CUTEFISH_SWITCHER_SWITCHER_VIEW_H_

#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <QImage>
#include <QObject>
#include <QRect>
#include <QTimer>

#include "switcher/application_info.h"
#include "wm/window_cycle_controller.h"

namespace cutefish::render {
class Renderer;
class Texture;
} // namespace cutefish::render

namespace cutefish::toolkit {
class PanelTexture;
}

namespace cutefish::ui {
class EmbeddedWindow;
}

namespace cutefish::decoration {
class ShadowNode;
class Theme;
} // namespace cutefish::decoration

namespace cutefish::scene {
class Node;
}

namespace cutefish::wm {
class WindowManager;
}

namespace cutefish::switcher {

class SwitcherContent;

// The application switcher's list: a row of application icons on a glass
// panel, centred on the output being switched on, with the selected one's
// name under it. Drawn by the compositor itself, as the lock screen is: a
// toolkit view tree rendered into an image the panel node composites over
// the backdrop blur. Upstream: WindowCycleView.
class SwitcherView : public QObject, public wm::WindowCycleView
{
    Q_OBJECT

public:
    // Logical pixels. Cells shrink together until the row fits the output.
    // The distance from one icon to the next: the icons' artwork, which themes
    // draw a few pixels inside the icon's square, stands a quarter of its
    // width apart. The selection hugs the artwork.
    static constexpr qreal kCellSize = 108;
    static constexpr qreal kIconRatio = 96.0 / 108;
    static constexpr qreal kSelectionRatio = 98.0 / 108;
    // Beside the row.
    static constexpr qreal kPadding = 16;
    // Above and below the row, alike, so the icons sit in the middle of the
    // panel as far from every edge; the selected one's name goes just under
    // its selection.
    static constexpr qreal kVerticalPadding = 18;
    static constexpr qreal kScreenMargin = 40;
    static constexpr int kRadius = 36;
    // At full size; it shrinks with the cells.
    static constexpr qreal kSelectionRadius = 22;

    SwitcherView(scene::Node *container, wm::WindowManager *manager,
                 const decoration::Theme *theme, QObject *parent = nullptr);
    ~SwitcherView() override;

    // Builds the view tree ahead of the first switch, which would otherwise
    // pay for the fonts and the icon theme while the list appears.
    // From then on the application of every window that maps is looked up and
    // its icon rendered, one window at a time while the compositor is idle.
    void preload();

    static constexpr int kWarmDelayMs = 200;

    // --- wm::WindowCycleView ---

    void showWindowCycle(const std::vector<wm::Window *> &entries, int selected,
                         const QString &output) override;
    void selectWindowCycleEntry(int index) override;
    void hideWindowCycle() override;
    bool windowCycleViewContains(const QPointF &location) const override;
    int windowCycleEntryAt(const QPointF &location) const override;

    // The panel's contents, rendered anew on the CPU when they changed.
    QImage renderImage();
    // In the panel node's paint: its contents as a texture of the renderer,
    // drawn on the GPU where the renderer allows.
    render::Texture *panelTexture(render::Renderer &renderer);
    void releaseRenderResources();
    quint64 imageGeneration() const { return m_imageGeneration; }
    QRect bounds() const { return m_bounds; }
    // The selected entry's own glass, in global logical coordinates; empty
    // with nothing selected.
    QRect selectionBounds() const;
    int selectionRadius() const { return qRound(kSelectionRadius * m_cellSize / kCellSize); }
    const decoration::Theme *theme() const { return m_theme; }

private:
    void ensureWindow();
    void markDirty();
    // The logical bounds and scale of the output named `output`.
    std::pair<QRect, double> outputFor(const QString &output) const;
    // Sizes the image and the views for `entries` centred on `area`.
    void layout(const std::vector<ApplicationInfo> &entries, int selected, const QRect &area,
                double scale);
    // The row's top in the panel, centred in it.
    qreal rowY() const { return (m_bounds.height() - m_cellSize) / 2; }
    // The icon size cells have at full size, in device pixels: icons are made
    // once at it and scaled into shrunken cells.
    static int iconPixels(double scale);
    void followIconTheme();
    void warmNext();

    wm::WindowManager *m_manager;
    const decoration::Theme *m_theme;
    ApplicationInfoCache m_applications;
    QString m_iconTheme;
    std::deque<quint64> m_warmQueue;
    QTimer m_warmTimer;
    std::unique_ptr<ui::EmbeddedWindow> m_window;
    std::unique_ptr<toolkit::PanelTexture> m_panelTexture;
    SwitcherContent *m_content = nullptr;
    double m_scale = 1;
    decoration::ShadowNode *m_shadow = nullptr;
    scene::Node *m_panel = nullptr;
    QRect m_bounds;
    qreal m_rowX = 0;
    qreal m_cellSize = kCellSize;
    int m_count = 0;
    int m_selected = -1;
    bool m_shown = false;
    bool m_dirty = false;
    quint64 m_imageGeneration = 0;
};

} // namespace cutefish::switcher

#endif // CUTEFISH_SWITCHER_SWITCHER_VIEW_H_
