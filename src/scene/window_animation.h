// Copyright 2013 The Chromium Authors
// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/window_animations.cc
//           chromium/src/ui/wm/core/window_animations.cc
//           chromium/src/ui/gfx/animation/tween.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#ifndef CUTEFISH_SCENE_WINDOW_ANIMATION_H_
#define CUTEFISH_SCENE_WINDOW_ANIMATION_H_

#include <functional>
#include <optional>
#include <vector>

#include <QImage>
#include <QRectF>

#include "scene/damage_tracker.h"
#include "scene/node.h"
#include "scene/window_visual_node.h"

namespace cutefish::scene {

// Presentation only: no Window, Wayland resource or input ownership survives
// in a closing animation. The parent scene owns the node until its last frame.
class WindowAnimation : public Node
{
public:
    enum class Type {
        Open,
        Close,
        Minimize,
        Restore,
        Bounds,
        // A bounds cross-fade whose destination is wherever the node is right
        // now, rather than a rectangle fixed when it started. It is what a
        // window dragged out of a maximized or snapped state needs: the old
        // appearance has to dissolve into the new one while the window keeps
        // following the pointer.
        // Upstream: CrossFadeAnimationAnimateNewLayerOnly and
        // CrossFadeUpdateTransformObserver in ash/wm/window_animations.cc.
        DragBounds
    };

    // Launcher is a full-screen shell surface: it fades through a slight zoom
    // instead of scaling a window rectangle up from its centre. Popup is a
    // menu or tooltip: a short pure fade in place, following the live
    // geometry, the way exo gives non-activatable windows
    // WINDOW_VISIBILITY_ANIMATION_TYPE_FADE. Fade is the desktop: a plain
    // opacity change over the window timings, with no motion at all, since a
    // full-screen surface scaling up from its centre only reads as a wobble.
    // None shows and removes the subtree immediately, ash's wm::ANIMATE_NONE.
    enum class Style {
        Window,
        Launcher,
        Popup,
        Fade,
        None
    };

    static constexpr bool isCrossFade(Type type)
    {
        return type == Type::Bounds || type == Type::DragBounds;
    }

    struct Sample {
        QRectF bounds;
        float opacity;
        double progress;
    };

    explicit WindowAnimation(std::function<double()> clock = {});
    void finish();
    void releaseRenderResources() override;
    void start(Type type, const QRect &from, const QRect &to, bool requestFrame = true);
    void close();
    void setBoundsContentReady(bool ready);

    // A launcher that has not painted yet takes over a launcher still closing
    // beside it, so reopening mid-close reverses instead of stacking two fades.
    void setStyle(Style style);

    Style style() const { return m_style; }

    // Where a running DragBounds animation is heading now. The drag moves the
    // window on every pointer motion, so the outgoing appearance has to be
    // re-aimed at it; it keeps the offset and the scale it started with, which
    // is what upstream's per-frame transform update does. It only re-aims: like
    // any cross-fade, the animation still waits for the client's content, so
    // nothing is drawn at a size the client has not reached. A no-op unless a
    // DragBounds animation is running.
    void setDragTarget(const QRect &bounds);

    bool isWaitingForBoundsContent() const { return !m_boundsContentReady; }

    // Sends a preview on the first minimize frame, then the exact landing image
    // before the finished callback. Both are cropped and capped to maximumPixels.
    void requestSnapshot(std::function<void(const QImage &)> callback, int maximumPixels);

    // The dock slot moved while a minimize runs toward it or holds on it.
    void setMinimizeTarget(const QRect &target);

    // A minimize that ends by holding its last frame on the dock slot, until
    // releaseHold(): the shell takes over with its thumbnail in its own commit,
    // so no frame shows both or neither.
    void setHoldAtEnd(bool hold) { m_holdAtEnd = hold; }
    void releaseHold();

    bool isHolding() const { return m_holding; }

    // Minimize and restore pass over the panels, so the window meets its dock
    // thumbnail instead of sliding under the dock.
    bool drawsAbovePanels() const { return isDockTransition(); }

    // Called from rendering when a running animation reaches its end.
    void setFinishedCallback(std::function<void()> callback) { m_finished = std::move(callback); }

    bool isRunning() const { return m_running; }

    Type type() const { return m_type; }

    // The client's whole-window opacity, over every draw of the window and
    // its animations. Its surfaces still get frames at zero.
    float opacity() const { return m_opacity; }

    void setOpacity(float opacity);

    void render(render::Renderer &renderer) const override;

    bool isRetired() const override { return m_closing && !m_running; }

    // The animation composites its subtree offscreen and draws the result
    // transformed or blended, so nothing under it can reach a hardware plane.
    bool paintsSubtree() const override { return animating() || m_opacity < 1; }

    // Other animations draw only inside absolutePaintBounds(). A cross-fade
    // scales the old snapshot's shadow up past them, and a minimize bends
    // toward a dock slot that moves.
    bool requiresFullRepaint() const override
    {
        return animating() && (isCrossFade(m_type) || m_type == Type::Minimize);
    }

    // A minimize plays on after its window is hidden, and so does a close.
    bool paintsWhenHidden() const override
    {
        return !isRetired()
                && (m_closing || (m_type == Type::Minimize && (m_running || m_holding)));
    }

    bool redrawsSubtree(const QRect &viewport, double scale) const override;

    Content content() const override { return Content::Translucent; }

    QRect absolutePaintBounds() const override;

    static Sample sample(Type type, const QRectF &from, const QRectF &to, double progress,
                         Style style = Style::Window);
    static int duration(Type type, const QRectF &from, const QRectF &to,
                        Style style = Style::Window);

private:
    // What this frame draws differs from the subtree as it stands. A static
    // opacity does not count: the snapshot is the same, only blended.
    bool animating() const { return m_running || m_holding || m_firstFrame || m_closing; }
    // Damages what this animation draws, rather than the whole window layer.
    void changed() const;
    Sample current() const;
    // The frame time of the output frame that drew this node last, or the
    // clock when that draw was not part of one.
    double now() const { return m_frameTime >= 0 ? m_frameTime : m_now(); }
    void readSnapshot(render::Renderer &renderer, bool landing = false) const;
    bool isDockTransition() const
    {
        return (m_running || m_holding) && m_style == Style::Window
                && (m_type == Type::Minimize || m_type == Type::Restore);
    }
    // The shadow and outline shrink away with the window, so what reaches the
    // dock is only the body the thumbnail shows.
    float exteriorOpacity(const Sample &sample) const;
    render::RoundedShape bodyShape(const QRect &original, const QRectF &bounds) const;
    static render::RoundedShape transformedShape(render::RoundedShape shape, const QRect &original,
                                                 const Sample &sample);
    void outline(render::Renderer &renderer, const WindowVisualState &visual, const QRect &original,
                 const Sample &sample) const;
    // Maximize and restore swap the window's exterior: a bounds cross-fade
    // carries its corners, shadow and outline over instead of switching them.
    bool morphsExterior() const;
    qreal morphedRadius(const Sample &sample) const;
    void drawExteriorMorph(render::Renderer &renderer, const Sample &value, float blend) const;
    mutable WindowVisualState m_visual;
    WindowVisualState m_oldVisual;
    void draw(render::Renderer &renderer, render::Texture *texture, const QRect &coverage,
              const QRect &original, const Sample &sample, float alpha = 1) const;
    // The subtree with its outlying subsurfaces and shadow overflow, which the
    // texture holds.
    QRect textureCoverage() const;
    // Whether m_texture was drawn for this coverage on the output with this
    // viewport and scale, and may be kept, apart from what changed since.
    bool textureReusable(const QRect &coverage, const QRect &viewport, double scale) const;
    // Whether m_texture still holds what the subtree would draw into it now, so
    // the offscreen pass can be skipped the way viz skips a render pass whose
    // contributing content has not changed (DirectRenderer::CanSkipRenderPass).
    // Otherwise `repaint` is the part to draw again, or empty for all of it.
    // Advances the texture's damage history either way.
    bool textureIsCurrent(render::Renderer &renderer, const QRect &coverage,
                          QRect *repaint) const;

    mutable std::unique_ptr<render::Texture> m_texture;
    // What m_texture was drawn from: the subtree's damage since, whether it may
    // be reused at all, its raster scale, the output's viewport and the scene
    // root's revision, which session-wide appearance changes bump.
    mutable DamageTracker m_textureDamage;
    mutable bool m_textureReusable = false;
    mutable double m_textureScale = 0;
    mutable QRect m_textureViewport;
    mutable uint64_t m_textureRootRevision = 0;
    mutable std::unique_ptr<render::Texture> m_landingTexture;
    mutable std::unique_ptr<render::Texture> m_oldTexture;
    mutable QRect m_coverage;
    mutable QRect m_textureBounds;
    QRect m_oldCoverage;
    QRect m_oldBounds;
    QRectF m_from;
    QRectF m_to;
    // Where the outgoing appearance sits relative to the live geometry, as an
    // origin offset and a size ratio. DragBounds re-derives m_from from these
    // on every frame, which is what keeps the dissolve under the pointer.
    QPointF m_dragOffset;
    QSizeF m_dragScale {1, 1};
    Type m_type = Type::Open;
    Style m_style = Style::Window;
    // The presented state of the launcher this one took over, relative to its snapshot.
    std::optional<Sample> m_handoff;
    std::function<double()> m_now;
    mutable double m_frameTime = -1;
    mutable double m_started = -1;
    mutable bool m_firstStep = false;
    mutable double m_firstStepAt = -1;
    mutable bool m_running = false;
    mutable bool m_firstFrame = true;
    bool m_closing = false;
    bool m_boundsContentReady = true;
    float m_startOpacity = 1;
    float m_opacity = 1;
    mutable Sample m_presented {{}, 1, 0};
    mutable std::function<void(const QImage &)> m_snapshot;
    mutable std::function<void(const QImage &)> m_landingSnapshot;
    int m_snapshotPixels = 0;
    std::function<void()> m_finished;
    bool m_holdAtEnd = false;
    mutable bool m_holding = false;
    // The window's own bounds and its dock slot, for the exterior fade.
    QRectF m_dockWindow;
    QRectF m_dockSlot;

    struct TargetAdjustment {
        QPointF position;
        QSizeF size;
        double progress;
    };

    std::vector<TargetAdjustment> m_minimizeAdjustments;
};

} // namespace cutefish::scene

#endif // CUTEFISH_SCENE_WINDOW_ANIMATION_H_
