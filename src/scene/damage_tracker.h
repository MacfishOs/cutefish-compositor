// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QRegion>
#include <functional>
#include <map>
#include <vector>

#include "render/damage.h"
#include "scene/node.h"
#include "scene/surface_node.h"

namespace cutefish::scene {

// Coverage history follows cc/trees/damage_tracker at Chromium 7acd40ddefff.
// Each output owns a history, including contributors outside its last damage.
class DamageTracker
{
public:
    // With `rootIsViewport`, the root's own geometry and revision are what
    // the viewport accounts for, as for a window whose texture spans it: a
    // move or a change of its own is not damage, only its children's are.
    explicit DamageTracker(bool rootIsViewport = false)
        : m_rootIsViewport(rootIsViewport)
    {
    }

    // A blur reads what lies behind it. `backdropChanged`, when given, says
    // whether that changed where the tree tracked here does not reach, as for
    // a window drawn offscreen over the output.
    QRegion update(const Node &root, const QRect &viewport, double scale = 1,
                   uint32_t transform = 0,
                   const std::function<bool(const QRect &)> &backdropChanged = {})
    {
        Frame frame = collect(root, viewport, scale, transform, backdropChanged);
        m_history = std::move(frame.history);
        m_viewport = viewport;
        m_samples = std::move(frame.samples);
        return frame.damage;
    }

    // Whether update() would find anything to repaint, leaving the history be.
    bool hasDamage(const Node &root, const QRect &viewport, double scale = 1,
                   uint32_t transform = 0) const
    {
        return !collect(root, viewport, scale, transform, {}).damage.isEmpty();
    }

    // What each blur found in the last update() reads, for the repaint to
    // cover whole wherever it reaches one.
    const std::vector<QRect> &samples() const { return m_samples; }

private:
    struct Entry {
        QRect bounds;
        uint64_t revision;
        uint64_t contentRevision;
    };

    struct Frame {
        QRegion damage;
        std::map<uint64_t, Entry> history;
        std::vector<QRect> samples;
    };

    // Bounds are kept relative to the viewport's origin: an output's viewport
    // stays put, while a window texture's moves with the window, whose subtree
    // then reads as unchanged, the way a cc render surface's damage is tracked
    // in the surface's own space and not the target's.
    Frame collect(const Node &root, const QRect &viewport, double scale, uint32_t transform,
                  const std::function<bool(const QRect &)> &backdropChanged) const
    {
        Frame frame;
        QRegion &damage = frame.damage;
        bool filtered = false;
        const QPoint origin = viewport.topLeft();
        const auto visit = [&](const auto &self, const Node &node, bool ancestorChanged,
                               bool redrawn) -> void {
            if (!node.isVisible() && !node.paintsWhenHidden())
                return;
            if (m_rootIsViewport && &node == &root) {
                for (const Node *child : node.children())
                    self(self, *child, false, false);
                return;
            }
            const QRect absolute = node.absolutePaintBounds();
            const Entry current {absolute.translated(-origin), node.revision(),
                                 node.contentRevision()};
            const auto previous = m_history.find(node.damageId());
            const bool changed = ancestorChanged || previous == m_history.end()
                    || previous->second.bounds != current.bounds
                    || previous->second.revision != current.revision;
            if (changed) {
                damage += current.bounds;
                if (previous != m_history.end())
                    damage += previous->second.bounds;
            } else {
                damage += expandedContentDamage(node,
                                                node.damageSince(previous->second.contentRevision))
                                  .translated(node.absolutePosition() - origin);
            }
            frame.history.emplace(node.damageId(), current);
            // Filter inputs and transformed subtrees need complete source pixels,
            // but only on the outputs they draw on.
            const auto *surface = dynamic_cast<const SurfaceNode *>(&node);
            filtered |= node.requiresFullRepaint() && absolute.intersects(viewport);
            if (surface && surface->hasBackgroundBlur()) {
                if (transform != 0 || scale <= 0) {
                    filtered |= absolute.intersects(viewport);
                } else {
                    // Every blur input, including the outward-rounded sample halo.
                    const int halo = render::kBackdropBlurSupport + int(std::ceil(2 / scale));
                    const QRect sampled = absolute.adjusted(-halo, -halo, halo, halo);
                    frame.samples.push_back(sampled);
                    if (redrawn || (backdropChanged && backdropChanged(sampled)))
                        damage += sampled.translated(-origin);
                }
            }
            // A blur drawn again from scratch reads everything behind it again.
            redrawn = redrawn || (&node != &root && node.redrawsSubtree(viewport, scale));
            for (const Node *child : node.children())
                self(self, *child, changed, redrawn);
        };
        visit(visit, root, false, false);
        for (const auto &[id, previous] : m_history)
            if (!frame.history.contains(id))
                damage += previous.bounds;
        if (m_history.empty() || viewport.size() != m_viewport.size() || filtered)
            damage = QRect(QPoint(), viewport.size());
        else
            damage = coverSamples(damage.translated(origin), frame.samples).translated(-origin);
        damage = damage.translated(origin).intersected(viewport);
        return frame;
    }

    // A blur changes with any of its backdrop, and redrawn anywhere it needs
    // all of it, so damage reaching what one reads takes in everything it reads.
    static QRegion coverSamples(QRegion damage, const std::vector<QRect> &samples)
    {
        for (bool grown = true; grown;) {
            grown = false;
            for (const QRect &sampled : samples) {
                if (damage.intersects(sampled)
                    && !QRegion(sampled).subtracted(damage).isEmpty()) {
                    damage += sampled;
                    grown = true;
                }
            }
        }
        return damage;
    }

    static QRegion expandedContentDamage(const Node &node, const QRegion &damage)
    {
        const auto *surface = dynamic_cast<const SurfaceNode *>(&node);
        if (!surface || !surface->texture() || node.geometry().isEmpty() || damage.isEmpty())
            return damage;
        const QSize size = node.geometry().size(), pixels = surface->texture()->size();
        const QTransform toBuffer = QTransform::fromScale(1.0 / size.width(), 1.0 / size.height())
                * surface->textureMapping()
                * QTransform::fromScale(pixels.width(), pixels.height());
        bool invertible = false;
        const QTransform toSurface = toBuffer.inverted(&invertible);
        const QRect bounds(QPoint(), size);
        if (!invertible)
            return bounds;
        QRegion result;
        // Linear sampling can change neighboring pixels beyond the damaged texel.
        for (const QRect &rect : damage.intersected(bounds)) {
            const QRectF source = toBuffer.mapRect(QRectF(rect)).adjusted(-1, -1, 1, 1);
            result += toSurface.mapRect(source).toAlignedRect().intersected(bounds);
        }
        return result;
    }

    bool m_rootIsViewport = false;
    std::map<uint64_t, Entry> m_history;
    QRect m_viewport;
    std::vector<QRect> m_samples;
};

} // namespace cutefish::scene
