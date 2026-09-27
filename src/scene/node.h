// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SCENE_NODE_H_
#define CUTEFISH_SCENE_NODE_H_

#include <functional>
#include <memory>
#include <vector>

#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QString>
#include <deque>

#include "render/renderer.h"

namespace cutefish::scene {

// A positioned, ordered item in the compositor scene. Geometry is relative to
// the parent; first child is furthest back. Children normally paint after the
// parent, with an optional split for subsurfaces stacked below parent content.
class Node
{
public:
    explicit Node(const QString &name = QString());
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    virtual ~Node();

    QString name() const { return m_name; }

    Node *parent() const { return m_parent; }

    const std::vector<Node *> &children() const { return m_childOrder; }

    Node *addChild(std::unique_ptr<Node> child);
    std::unique_ptr<Node> removeChild(Node *child);

    // Moves `child` to the front (top) or back (bottom) of the sibling order.
    void raise(Node *child);
    void lower(Node *child);

    // Puts the children in `order`, which has to list each of them once. Only
    // the ones that have to move change, as aura restacks one window at a time
    // (Window::StackChildAbove): the longest run already in order stays put.
    void reorderChildren(const std::vector<Node *> &order);

    void setChildrenBelowParent(size_t count);

    size_t childrenBelowParent() const { return m_childrenBelowParent; }

    QRect geometry() const { return m_geometry; }

    void setGeometry(const QRect &geometry);

    QPoint position() const { return m_geometry.topLeft(); }

    void setPosition(const QPoint &position) { setGeometry(QRect(position, m_geometry.size())); }

    // Position in output coordinates.
    QPoint absolutePosition() const;
    QRect absoluteGeometry() const;

    virtual QRect absolutePaintBounds() const { return absoluteGeometry(); }

    bool isVisible() const { return m_visible; }

    void setVisible(bool visible);

    void setChangeCallback(std::function<void()> callback) { m_changed = std::move(callback); }

    void notifyChanged() const;
    void notifyDamage(const QRegion &damage);

    // Runs the change callbacks without changing anything, for a hidden child
    // that still paints. Nothing runs under a hidden ancestor.
    void requestFrame() const;

    uint64_t damageId() const { return m_damageId; }

    uint64_t revision() const { return m_revision; }

    uint64_t contentRevision() const { return m_contentRevision; }

    QRegion damageSince(uint64_t revision) const;

    // True only if this node and every ancestor is visible.
    bool isEffectivelyVisible() const;

    // Paints children on either side of this node, skipping invisible subtrees.
    virtual void render(render::Renderer &renderer) const;

    // What this node's own paint() puts inside geometry(). The scanout planner
    // needs it to know what a hardware plane would have to replace.
    enum class Content {
        None, // paints nothing of its own
        Opaque, // covers geometry() with no transparency
        Translucent, // paints, possibly blended
    };

    virtual Content content() const { return Content::None; }

    // The node draws its whole subtree itself - an offscreen pass, a transform,
    // an animation - so the planner must treat the subtree as one blob rather
    // than descending into it. Nothing inside can go on a hardware plane.
    virtual bool paintsSubtree() const { return false; }

    virtual bool requiresFullRepaint() const { return paintsSubtree(); }

    // A hidden node that still draws, such as an animation playing on after
    // its window is gone. Damage follows it the same as a visible one.
    virtual bool paintsWhenHidden() const { return false; }

    // Whether an output with this viewport and scale is about to draw the
    // subtree again whole, as into a texture kept between frames that no
    // longer holds it, rather than only where it changed.
    virtual bool redrawsSubtree(const QRect &, double) const { return false; }

    virtual bool acceptsInput(const QPointF &) const { return true; }

    // Drawn as usual, but left out of the input hit test, as aura's
    // ScopedWindowEventTargetingBlocker does for a window dragged with a drag.
    bool isEventTargetingBlocked() const { return m_eventTargetingBlocked; }

    void setEventTargetingBlocked(bool blocked) { m_eventTargetingBlocked = blocked; }

    // Paints over the whole of the renderer's current output, opaquely, so
    // nothing drawn before it there can show. Its parent then skips those
    // siblings, as viz drops quads an opaque one occludes.
    virtual bool occludesOutput(const render::Renderer &) const { return false; }
    // A visible node in this subtree occludes the output.
    bool subtreeOccludesOutput(const render::Renderer &renderer) const;

    // Set by the display controller for content it has moved onto a hardware
    // plane, so composition leaves the same pixels out.
    bool isPromotedToOverlay() const { return m_promotedToOverlay; }

    void setPromotedToOverlay(bool promoted) { m_promotedToOverlay = promoted; }

    // On screen, but never in an image or a video the compositor captures.
    bool isExcludedFromCapture() const { return m_excludedFromCapture; }

    void setExcludedFromCapture(bool excluded) { m_excludedFromCapture = excluded; }

    // render() for a capture: children excluded from capture are left out.
    void renderForCapture(render::Renderer &renderer) const;

    virtual bool isRetired() const { return false; }

    void collectRetired();
    virtual void releaseRenderResources();

protected:
    virtual void paint(render::Renderer &renderer, const QRect &absoluteGeometry) const;

private:
    // The topmost child that occludes the output, or 0 when none does.
    size_t firstUnoccludedChild(const render::Renderer &renderer) const;

    const uint64_t m_damageId;
    mutable uint64_t m_revision = 0;
    uint64_t m_contentRevision = 0;
    std::deque<std::pair<uint64_t, QRegion>> m_damageHistory;
    std::function<void()> m_changed;
    QString m_name;
    Node *m_parent = nullptr;
    std::vector<std::unique_ptr<Node>> m_children;
    // Paint order; mirrors m_children but is reordered by raise()/lower().
    std::vector<Node *> m_childOrder;
    QRect m_geometry;
    bool m_visible = true;
    bool m_promotedToOverlay = false;
    bool m_eventTargetingBlocked = false;
    bool m_excludedFromCapture = false;
    size_t m_childrenBelowParent = 0;
};

// Solid colour rectangle.
class ColorNode : public Node
{
public:
    explicit ColorNode(const render::Color &color, const QString &name = QString());

    render::Color color() const { return m_color; }

    void setColor(const render::Color &color);

    Content content() const override
    {
        if (m_color.a <= 0.0f)
            return Content::None;
        return m_color.a >= 1.0f ? Content::Opaque : Content::Translucent;
    }

protected:
    void paint(render::Renderer &renderer, const QRect &absoluteGeometry) const override;

private:
    render::Color m_color;
};

// Textured quad. The texture is not owned.
class TextureNode : public Node
{
public:
    explicit TextureNode(render::Texture *texture, const QString &name = QString());

    render::Texture *texture() const { return m_texture; }

    void setTexture(render::Texture *texture)
    {
        m_texture = texture;
        notifyChanged();
    }

    float alpha() const { return m_alpha; }

    void setAlpha(float alpha)
    {
        if (m_alpha != alpha) {
            m_alpha = alpha;
            notifyChanged();
        }
    }

    Content content() const override { return m_texture ? Content::Translucent : Content::None; }

protected:
    void paint(render::Renderer &renderer, const QRect &absoluteGeometry) const override;

private:
    render::Texture *m_texture = nullptr;
    float m_alpha = 1.0f;
};

} // namespace cutefish::scene

#endif // CUTEFISH_SCENE_NODE_H_
