// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "render/output_render_context.h"
#include <QRegion>
#include <cmath>
#include <deque>
#include <optional>
#include <vector>

namespace cutefish::render {

inline constexpr int kBackdropBlurSupport = 90;

inline QRect physicalDamage(const QRegion &damage, const OutputRenderContext &context,
                            const QSize &targetSize)
{
    const QRect target(QPoint(), targetSize);
    const QRect content = context.nativeContentRect.isEmpty() ? target : context.nativeContentRect;
    if (context.transform > 3 || context.scale <= 0)
        return target;
    // drawTextureMapped rounds a native-density texture's origin and size
    // apart, which can reach one pixel past the enclosing rect on the far side.
    const qreal overhang = context.scale == std::floor(context.scale) ? 0 : 1;
    QRegion result;
    for (const QRect &rect : damage.intersected(context.logicalViewport)) {
        QRectF local(rect.translated(-context.logicalViewport.topLeft()));
        local = QRectF(local.topLeft() * context.scale,
                       local.size() * context.scale + QSizeF(overhang, overhang));
        const double w = content.width(), h = content.height();
        switch (context.transform) {
        case 1:
            local = QRectF(local.y(), h - local.x() - local.width(), local.height(), local.width());
            break;
        case 2:
            local = QRectF(w - local.x() - local.width(), h - local.y() - local.height(),
                           local.width(), local.height());
            break;
        case 3:
            local = QRectF(w - local.y() - local.height(), local.x(), local.height(),
                           local.width());
            break;
        }
        // Enclose after transforming; adjacent fractional edges must never lose pixels.
        result += local.translated(content.topLeft()).toAlignedRect().intersected(content);
    }
    return result.boundingRect().intersected(target);
}

// The logical rectangle a physical repaint takes in, for culling: anything
// physicalDamage() maps into `repaint` comes from inside it. None for flips.
inline std::optional<QRect> logicalRepaint(const QRect &repaint,
                                           const OutputRenderContext &context,
                                           const QSize &targetSize)
{
    if (context.transform > 3 || context.scale <= 0)
        return std::nullopt;
    const QRect content = context.nativeContentRect.isEmpty() ? QRect(QPoint(), targetSize)
                                                              : context.nativeContentRect;
    QRectF local(repaint.translated(-content.topLeft()));
    const double w = content.width(), h = content.height();
    switch (context.transform) {
    case 1:
        local = QRectF(h - local.y() - local.height(), local.x(), local.height(), local.width());
        break;
    case 2:
        local = QRectF(w - local.x() - local.width(), h - local.y() - local.height(),
                       local.width(), local.height());
        break;
    case 3:
        local = QRectF(local.y(), w - local.x() - local.width(), local.height(), local.width());
        break;
    }
    local = QRectF(local.topLeft() / context.scale, local.size() / context.scale);
    // A pixel more on every side covers the overhang and the rounding out.
    return local.toAlignedRect()
            .adjusted(-1, -1, 1, 1)
            .translated(context.logicalViewport.topLeft());
}

// A backdrop blur reads everything in its `samples` region, and outside the
// repaint the target holds a finished frame, not the backdrop. A repaint that
// reaches such a region, if only through a bounding rectangle or the repair
// of an older buffer, takes in all of it.
inline QRect coverSamples(QRect repaint, const std::vector<QRect> &samples)
{
    for (bool grown = true; grown && !repaint.isEmpty();) {
        grown = false;
        for (const QRect &sampled : samples) {
            if (repaint.intersects(sampled) && !repaint.contains(sampled)) {
                repaint = repaint.united(sampled);
                grown = true;
            }
        }
    }
    return repaint;
}

// The same for a physical repaint and logical sample regions.
inline QRect coverSamples(const QRect &repaint, const std::vector<QRect> &samples,
                          const OutputRenderContext &context, const QSize &targetSize)
{
    std::vector<QRect> physical;
    for (const QRect &sampled : samples)
        physical.push_back(physicalDamage(sampled, context, targetSize));
    return coverSamples(repaint, physical);
}

class BufferDamageHistory
{
public:
    QRect repair(const QRect &damage, int age, const QRect &full) const
    {
        if (age <= 0 || size_t(age - 1) > m_history.size())
            return full;
        QRect result = damage;
        for (int i = 0; i < age - 1; ++i)
            result = result.united(m_history[i]);
        return result.intersected(full);
    }

    void presented(const QRect &damage)
    {
        m_history.push_front(damage);
        if (m_history.size() > 16)
            m_history.pop_back();
    }

    void reset() { m_history.clear(); }

private:
    std::deque<QRect> m_history;
};

} // namespace cutefish::render
