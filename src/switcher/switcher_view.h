// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SWITCHER_SWITCHER_VIEW_H_
#define CUTEFISH_SWITCHER_SWITCHER_VIEW_H_

#include <deque>
#include <memory>
#include <utility>

#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QVariantList>

#include "switcher/application_info.h"
#include "wm/window_cycle_controller.h"

class QQmlEngine;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;

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

// Application icons rendered at the size the list draws them. An SVG icon
// takes a few milliseconds to rasterize, so they are made ahead of the list.
class IconCache
{
public:
    // The icon named by a theme name or an absolute path, `pixels` square.
    QImage image(const QString &name, int pixels);
    void clear() { m_images.clear(); }

private:
    // Costed in bytes; a session's worth of applications at two scales fits.
    QCache<QString, QImage> m_images {8 * 1024 * 1024};
};

// The application switcher's list: a row of application icons on a glass
// panel, centred on the output being switched on, with the selected one's
// name under it. Drawn by the compositor itself, as the lock screen is: QML
// rendered offscreen into an image the panel node composites over the
// backdrop blur. Upstream: WindowCycleView.
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

    // Builds the QML scene ahead of the first switch, which would otherwise
    // pay for the engine, the fonts and the icon theme while the list appears.
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

    // The panel's contents, rendered anew when they changed.
    QImage renderImage();
    quint64 imageGeneration() const { return m_imageGeneration; }
    QRect bounds() const { return m_bounds; }
    // The selected entry's own glass, in global logical coordinates; empty
    // with nothing selected.
    QRect selectionBounds() const;
    int selectionRadius() const { return qRound(kSelectionRadius * m_cellSize / kCellSize); }
    const decoration::Theme *theme() const { return m_theme; }

private:
    bool ensureScene();
    void markDirty();
    // The logical bounds and scale of the output named `output`.
    std::pair<QRect, double> outputFor(const QString &output) const;
    // Sizes the image and the scene for `entries` centred on `area`.
    void layout(const QVariantList &entries, int selected, const QRect &area, double scale);
    void render();
    // The row's top in the panel, centred as the QML centres it.
    qreal rowY() const { return (m_bounds.height() - m_cellSize) / 2; }
    // The icon size cells have at full size, in device pixels: what Image
    // asks the provider for.
    static int iconPixels(double scale);
    void followIconTheme();
    void warmNext();

    wm::WindowManager *m_manager;
    const decoration::Theme *m_theme;
    ApplicationInfoCache m_applications;
    // Before the engine, whose icon provider reads it.
    IconCache m_icons;
    QString m_iconTheme;
    std::deque<quint64> m_warmQueue;
    QTimer m_warmTimer;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QQuickRenderControl> m_control;
    std::unique_ptr<QQuickWindow> m_window;
    QQuickItem *m_root = nullptr;
    decoration::ShadowNode *m_shadow = nullptr;
    scene::Node *m_panel = nullptr;
    QImage m_image;
    QRect m_bounds;
    qreal m_rowX = 0;
    qreal m_cellSize = kCellSize;
    int m_count = 0;
    int m_selected = -1;
    bool m_shown = false;
    bool m_dirty = false;
    bool m_rendering = false;
    quint64 m_imageGeneration = 0;
};

} // namespace cutefish::switcher

#endif // CUTEFISH_SWITCHER_SWITCHER_VIEW_H_
