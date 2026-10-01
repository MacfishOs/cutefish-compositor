// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SCENE_OVERVIEW_NODE_H_
#define CUTEFISH_SCENE_OVERVIEW_NODE_H_

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QImage>
#include <QRectF>
#include <QString>

#include "scene/finger_track.h"
#include "scene/motion.h"
#include "scene/node.h"

namespace cutefish::scene {

class WindowAnimation;

// One line of text at `density` device pixels to a logical one, no wider than
// `maxWidth` logical pixels: premultiplied RGBA8888, or null.
using OverviewTextPainter = std::function<QImage(const QString &text, int pixelSize,
                                                 const render::Color &color, double density,
                                                 int maxWidth)>;

// What the overview draws around every window alike, whatever its own state:
// the corners of a window in its normal state, `radius` logical pixels scaled
// with the window, a shadow under the body and an outline over it. `uniform`
// goes from 0, where the window's own shadow, which the painter reads off
// `window`, is drawn, to 1, where only the overview's is.
struct OverviewWindowFrame {
    qreal radius = 0;
    std::function<void(render::Renderer &renderer, const WindowAnimation &window,
                       const render::RoundedShape &body, double uniform, float opacity)>
            shadow;
    std::function<void(render::Renderer &renderer, const render::RoundedShape &body,
                       float opacity)>
            outline;
};

// Textures of the overview's text and glyphs, made once per look and scale.
class OverviewChromeCache
{
public:
    render::Texture *label(render::Renderer &renderer, const OverviewTextPainter &painter,
                           const QString &text, int pixelSize, const render::Color &color,
                           int maxWidth, QSizeF *size);
    enum class Glyph {
        Close,
        Plus,
    };
    // The glyph filling `size` logical pixels, drawn in `color`.
    render::Texture *glyph(render::Renderer &renderer, Glyph glyph, const QSizeF &size,
                           const render::Color &color);
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        QString key;
        std::unique_ptr<render::Texture> texture;
        QSizeF size;
    };
    render::Texture *find(const QString &key, QSizeF *size);
    render::Texture *insert(render::Renderer &renderer, const QString &key, const QImage &image,
                            const QSizeF &size);
    std::list<Entry> m_entries;
};

// The window overview's windows, over the desktop dimmed behind them. It sits
// above the active desk's windows and draws them itself, each moved from
// where it is towards its place in the overview by the overview's progress:
// 0 is the desk as it is, 1 the overview. Everything below it in its parent
// is covered while it is active; the panels and the dock above stay.
//
// The progress follows the fingers of a touchpad swipe, resampled at frame
// time as the desk swipe is, pulls against 0 and 1 like a rubber band and
// lets go into a critically damped spring keeping the fingers' speed; the
// shortcut rides the same spring. Upstream Ash interpolates cached transforms
// by the scroll ratio (OverviewGrid::PositionWindowsContinuously) and decides
// the release by distance alone.
class OverviewNode : public Node
{
public:
    struct Item {
        // Drawn together, bottom to top: a window and its transient children.
        // The first one's geometry is what `to` places.
        std::vector<const WindowAnimation *> windows;
        QRectF to;
        QString title;
    };

    // What the pointer or the keyboard is on.
    struct Chrome {
        int hovered = -1;
        int selected = -1;
    };

    // Critically damped springs, response in seconds: from a shortcut or a
    // click, once the fingers let go, and a window moving to a new place.
    static constexpr double kShowResponse = 0.45;
    static constexpr double kSettleResponse = 0.4;
    static constexpr double kLayoutResponse = 0.35;
    // Released faster than this, in progress per second, the overview goes on
    // the way the fingers went; slower, to the nearer end.
    static constexpr double kFlickVelocity = 0.5;
    // How far past either end the progress gives, at most, and how much of
    // the fingers' pull it follows at first.
    static constexpr double kRubberBandEdge = 0.1;
    static constexpr double kRubberBandCoefficient = 0.55;
    static constexpr double kChromeFadeMs = 150;
    static constexpr float kDimOpacity = 0.35f;

    explicit OverviewNode(std::function<qint64()> clock = {});

    void setTextPainter(OverviewTextPainter painter) { m_textPainter = std::move(painter); }
    void setWindowFrame(OverviewWindowFrame frame) { m_frame = std::move(frame); }

    // The desktop, drawn under the dim.
    void setBackdrop(std::vector<const Node *> nodes);

    // `animate` moves windows already shown to their new places on a spring;
    // otherwise they are there at once.
    void setItems(std::vector<Item> items, bool animate);
    const std::vector<Item> &items() const { return m_items; }

    // The item drawn at `rect` as the pointer drags it; none for -1. It is not
    // drawn with the rest but by renderDragged(), over the desk bar.
    void setDragged(int index, const QRectF &rect);
    void renderDragged(render::Renderer &renderer) const;

    // The item drawn over the rest; none for -1.
    void setRaised(int index);

    void setChrome(const Chrome &chrome);

    // --- Progress ---

    // Where the overview is at `timeMs` on the frame clock, between 0 and 1
    // but for the rubber band.
    double progress(double timeMs) const;
    double progress() const { return progress(clockTime()); }
    // The node's clock, for times outside an output frame.
    double clockTime() const { return double(m_now()); }

    // Heads for `target`, from where it is shown and at the speed it moves.
    void animateTo(double target);
    // Stands at `value` at once.
    void jumpTo(double value);
    double target() const;

    // The fingers take the progress where it is shown; `delta` moves it.
    void startTracking();
    void updateTracking(double delta, double timeMs);
    // The fingers let go at `timeMs`; returns where the progress heads, 0 or 1.
    double endTracking(double timeMs);
    bool isTracking() const { return m_motion == Motion::Tracking; }
    // Standing still at target().
    bool isSettled() const { return m_motion == Motion::Still; }

    // Shown at all: moving, or standing anywhere but 0.
    bool isActive() const { return m_motion != Motion::Still || m_position > 0; }

    // Runs from inside render() when the progress lands on 0 or 1, with that
    // value: must not change the scene.
    void setFinishedCallback(std::function<void(double)> callback)
    {
        m_finished = std::move(callback);
    }

    // Where item `index` is drawn at `progress`.
    QRectF presentedRect(int index, double progress, double timeMs) const;

    void render(render::Renderer &renderer) const override;
    Content content() const override { return isActive() ? Content::Opaque : Content::None; }
    bool paintsSubtree() const override { return isActive(); }
    bool requiresFullRepaint() const override { return isActive(); }
    bool acceptsInput(const QPointF &) const override { return false; }
    bool occludesOutput(const render::Renderer &) const override { return isActive(); }
    void releaseRenderResources() override;

private:
    enum class Motion {
        Still,
        Tracking,
        Spring,
    };

    double rubberBand(double pulled) const;
    double pulledPosition(double shown) const;
    void settle(double now, double from, double target, double velocity, double response);
    void updateCoast();
    double layoutProgress(double timeMs) const;
    // Whether the chrome shows, and how much, at `timeMs`.
    float chromeOpacity(double timeMs) const;
    void renderItem(render::Renderer &renderer, int index, double progress, double now) const;
    // The main window's body at `rect`, at rest in the overview.
    render::RoundedShape restingBody(int index, const QRectF &rect) const;
    void renderChrome(render::Renderer &renderer, const QRect &viewport, double now) const;
    void landed(double now) const;

    std::function<qint64()> m_now;
    OverviewTextPainter m_textPainter;
    OverviewWindowFrame m_frame;
    std::vector<const Node *> m_backdrop;
    std::vector<Item> m_items;
    // Where each item was headed when the layout last changed.
    std::vector<QRectF> m_layoutFrom;
    std::optional<motion::Spring> m_layout;
    int m_dragged = -1;
    QRectF m_dragRect;
    int m_raised = -1;
    Chrome m_chrome;

    mutable Motion m_motion = Motion::Still;
    mutable double m_position = 0;
    motion::FingerTrack m_fingers;
    std::optional<motion::Spring> m_coast;
    double m_coastStart = 0;
    motion::Spring m_spring;
    // When the progress came to rest at 1, for the chrome to fade in from.
    mutable double m_shownAt = -1;
    std::function<void(double)> m_finished;
    mutable OverviewChromeCache m_chromeCache;
};

// The overview's desk bar along the top of each output's work area: a preview
// of every desk and a new-desk button, even with one desk (Ash shows a strip
// of buttons until there are two). It slides in from the top edge with the
// overview's progress and stays still above a desk switch, which slides the
// overview's windows underneath.
class OverviewDeskBarNode : public Node
{
public:
    struct Desk {
        QRectF bounds;
        QString name;
        bool active = false;
        // The pointer is on it, or a window dragged over it would drop there.
        bool highlighted = false;
        // The pointer is on it and it can be removed: its close button shows,
        // lit while the pointer is on the button.
        bool closeShown = false;
        bool closeHovered = false;
    };

    struct Bar {
        // The output it is on, and where it sits once shown.
        QRect output;
        QRectF bounds;
        std::vector<Desk> desks;
        QRectF newDesk;
        QString newDeskLabel;
        bool newDeskHighlighted = false;
    };

    // What a desk preview shows: the desk's windows bottom to top, over the backdrop.
    struct DeskContent {
        std::vector<const WindowAnimation *> windows;
    };

    // Ash's DeskActionView close button, in the preview's top right corner.
    static constexpr double kCloseButtonSize = 24;
    static constexpr double kCloseButtonMargin = 4;
    static QRectF closeButtonRect(const QRectF &preview);

    static constexpr double kPreviewRadius = 8;
    static constexpr int kLabelPixelSize = 13;
    // A live desk preview is drawn again at most this often, and no more
    // previews than this in one frame.
    static constexpr double kPreviewRefreshMs = 500;
    static constexpr int kPreviewsPerFrame = 1;

    explicit OverviewDeskBarNode(const OverviewNode *overview);

    void setTextPainter(OverviewTextPainter painter) { m_textPainter = std::move(painter); }
    void setBars(std::vector<Bar> bars);
    void setContent(std::vector<const Node *> backdrop, std::vector<DeskContent> desks);
    // Draws every preview again at the next frame.
    void invalidatePreviews();

    // How far the bar on an output is slid up out of view at `progress`.
    static double slideOffset(const Bar &bar, double progress);

    void render(render::Renderer &renderer) const override;
    Content content() const override
    {
        return m_overview->isActive() ? Content::Translucent : Content::None;
    }
    bool paintsSubtree() const override { return m_overview->isActive(); }
    bool requiresFullRepaint() const override { return m_overview->isActive(); }
    bool acceptsInput(const QPointF &) const override { return false; }
    void releaseRenderResources() override;

private:
    struct Preview {
        std::unique_ptr<render::Texture> texture;
        QRectF bounds;
        double scale = 0;
        double drawnAt = -1;
        bool stale = true;
    };

    void renderBar(render::Renderer &renderer, const Bar &bar, double progress,
                   double now) const;
    void updatePreview(render::Renderer &renderer, const Bar &bar, int desk, double now) const;

    const OverviewNode *m_overview = nullptr;
    OverviewTextPainter m_textPainter;
    std::vector<Bar> m_bars;
    std::vector<const Node *> m_backdrop;
    std::vector<DeskContent> m_desks;
    // Keyed by the output's bounds and the desk.
    mutable std::map<std::pair<QString, int>, Preview> m_previews;
    mutable int m_previewsLeft = 0;
    mutable OverviewChromeCache m_chromeCache;
};

} // namespace cutefish::scene

#endif // CUTEFISH_SCENE_OVERVIEW_NODE_H_
