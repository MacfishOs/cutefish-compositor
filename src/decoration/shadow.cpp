// Copyright 2012 The Chromium Authors
// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/gfx/shadow_value.cc
//           chromium/src/ui/compositor_extra/{shadow,decoration_util}.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#include "decoration/shadow.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "render/rounded_shape.h"
#include "render/shape_mask.h"

namespace cutefish::decoration {

namespace {

struct Run {
    qreal begin = 0;
    qreal end = 0;
};

// The longest range of `vertical` rows (or columns) across which every row of
// the flattened outline is the same: no edge but ones parallel to the run
// passes through it, and no edge across it starts or ends inside.
Run longestStraightRun(const QList<QPolygonF> &polygons, bool vertical)
{
    const auto along = [vertical](const QPointF &p) {
        return vertical ? p.y() : p.x();
    };
    const auto across = [vertical](const QPointF &p) {
        return vertical ? p.x() : p.y();
    };
    std::vector<qreal> stops;
    std::vector<Run> slanted;
    std::vector<qreal> flat;
    for (const auto &polygon : polygons) {
        for (int i = 0; i < polygon.size(); ++i) {
            const QPointF a = polygon[i];
            const QPointF b = polygon[(i + 1) % polygon.size()];
            stops.push_back(along(a));
            if (qFuzzyCompare(along(a) + 1, along(b) + 1))
                flat.push_back(along(a));
            else if (!qFuzzyCompare(across(a) + 1, across(b) + 1))
                slanted.push_back({std::min(along(a), along(b)), std::max(along(a), along(b))});
        }
    }
    std::sort(stops.begin(), stops.end());
    Run best;
    Run current;
    bool open = false;
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const Run interval {stops[i], stops[i + 1]};
        if (interval.end - interval.begin < 1e-9)
            continue;
        const bool straight = std::none_of(slanted.begin(), slanted.end(), [&](const Run &edge) {
            return edge.begin < interval.end - 1e-9 && edge.end > interval.begin + 1e-9;
        });
        const bool joined = open && std::none_of(flat.begin(), flat.end(), [&](qreal y) {
                                return std::abs(y - interval.begin) < 1e-9;
                            });
        if (!straight) {
            open = false;
            continue;
        }
        current = joined ? Run {current.begin, interval.end} : interval;
        open = true;
        if (current.end - current.begin > best.end - best.begin)
            best = current;
    }
    return best;
}

} // namespace

ShadowGeometry::ShadowGeometry(const QSize &windowSize, int radius, const ShadowStyle &style,
                               int rasterScale, const Corners &corners,
                               const QPainterPath &silhouette)
    : m_radius(radius)
    , m_scale(rasterScale)
    , m_corners(corners)
    , m_style(style)
    , m_silhouette(silhouette)
{
    // Internal assets have a bounded physical footprint. Scale 1 is the output
    // contract today; integer raster scales 2..4 are exercised independently.
    if (windowSize.isEmpty() || windowSize.width() > (1 << 20) || windowSize.height() > (1 << 20)
        || rasterScale < 1 || rasterScale > 4 || radius < 0 || radius > kMaxRadius
        || radius > windowSize.width() / 2 || radius > windowSize.height())
        return;

    int support = 0;
    int offset = 0;
    for (const auto &layer : style) {
        if (!std::isfinite(layer.sigma) || layer.sigma < 0 || layer.sigma > 32
            || (layer.offset.x() < -32 || layer.offset.x() > 32)
            || (layer.offset.y() < -32 || layer.offset.y() > 32) || !std::isfinite(layer.opacity)
            || layer.opacity < 0 || layer.opacity > 1)
            return;
        // Adapt GetMargin's union of offset exterior extents. Our discrete
        // Gaussian is explicitly truncated at 3 sigma, not blur()/2.
        const int reach = static_cast<int>(std::ceil(3 * layer.sigma));
        m_margins = QMargins(std::max(m_margins.left(), reach - layer.offset.x()),
                             std::max(m_margins.top(), reach - layer.offset.y()),
                             std::max(m_margins.right(), reach + layer.offset.x()),
                             std::max(m_margins.bottom(), reach + layer.offset.y()));
        support = std::max(support, reach);
        offset = std::max({offset, std::abs(layer.offset.x()), std::abs(layer.offset.y())});
    }

    m_windowSize = windowSize;
    if (!m_silhouette.isEmpty()) {
        // Rows within this far of anything that is not straight see it through the blur.
        const int reach = support + offset + 1;
        const auto polygons = m_silhouette.toSubpathPolygons();
        const auto stretch = [&](bool vertical, int extent) {
            const Run run = longestStraightRun(polygons, vertical);
            const int begin = int(std::ceil(run.begin)) + reach;
            const int end = int(std::floor(run.end)) - reach;
            // Stretching fewer than two rows saves nothing.
            return end - begin >= 2 ? std::pair {begin, end} : std::pair {extent, extent};
        };
        const auto [top, bottom] = stretch(true, windowSize.height());
        const auto [left, right] = stretch(false, windowSize.width());
        m_stretchBegin = QPoint(left, top);
        m_stretchEnd = QPoint(right, bottom);
        m_canonicalSize =
                windowSize - QSize(right - left - (right > left), bottom - top - (bottom > top));
        const QSize imageSize = m_canonicalSize
                + QSize(m_margins.left() + m_margins.right(), m_margins.top() + m_margins.bottom());
        m_rasterSize = imageSize * m_scale;
        const QSize targetSize = paintBounds().size();
        const auto cuts = [](int margin, int begin, int end, int image, int target) {
            const int sample = end > begin ? 1 : 0;
            return std::pair {std::array {0, margin + begin, margin + begin + sample, image},
                              std::array {0, margin + begin, margin + end, target}};
        };
        const auto [sx, dx] =
                cuts(m_margins.left(), left, right, imageSize.width(), targetSize.width());
        const auto [sy, dy] =
                cuts(m_margins.top(), top, bottom, imageSize.height(), targetSize.height());
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                m_sources[y * 3 + x] =
                        QRect(sx[x] * m_scale, sy[y] * m_scale, (sx[x + 1] - sx[x]) * m_scale,
                              (sy[y + 1] - sy[y]) * m_scale);
                m_targets[y * 3 + x] = QRect(dx[x], dy[y], dx[x + 1] - dx[x], dy[y + 1] - dy[y])
                                               .translated(-m_margins.left(), -m_margins.top());
            }
        }
        return;
    }
    // Full blur support plus corner space before the one-logical-pixel
    // aperture, with an extra sample of constant edge for linear filtering.
    const int fixed = int(std::ceil(m_radius * 1.5)) + support + offset + 1;
    m_canonicalSize = QSize(std::min(windowSize.width(), 2 * fixed + 1),
                            std::min(windowSize.height(), 2 * fixed + 1));
    const int left = m_margins.left() + std::min(fixed, m_canonicalSize.width() / 2);
    const int top = m_margins.top() + std::min(fixed, m_canonicalSize.height() / 2);
    const int right = m_margins.right() + std::min(fixed, m_canonicalSize.width() / 2);
    const int bottom = m_margins.bottom() + std::min(fixed, m_canonicalSize.height() / 2);
    const QSize imageSize = m_canonicalSize
            + QSize(m_margins.left() + m_margins.right(), m_margins.top() + m_margins.bottom());
    m_rasterSize = imageSize * m_scale;
    const QSize targetSize = paintBounds().size();
    const int sx[] = {0, left, imageSize.width() - right, imageSize.width()};
    const int sy[] = {0, top, imageSize.height() - bottom, imageSize.height()};
    const int dx[] = {0, left, targetSize.width() - right, targetSize.width()};
    const int dy[] = {0, top, targetSize.height() - bottom, targetSize.height()};
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            m_sources[y * 3 + x] =
                    QRect(sx[x] * m_scale, sy[y] * m_scale, (sx[x + 1] - sx[x]) * m_scale,
                          (sy[y + 1] - sy[y]) * m_scale);
            m_targets[y * 3 + x] = QRect(dx[x], dy[y], dx[x + 1] - dx[x], dy[y + 1] - dy[y])
                                           .translated(-m_margins.left(), -m_margins.top());
        }
    }
}

QRect ShadowGeometry::paintBounds() const
{
    return m_windowSize.isEmpty() ? QRect() : QRect(QPoint(), m_windowSize) + m_margins;
}

QString ShadowGeometry::key() const
{
    QString result = QStringLiteral("shadow-v5:%1:%2:%3:%4:%5:%6%7%8%9")
                             .arg(m_canonicalSize.width())
                             .arg(m_canonicalSize.height())
                             .arg(m_radius)
                             .arg(m_scale)
                             .arg(render::continuousCorners() ? 1 : 0)
                             .arg(m_corners.topLeft ? 1 : 0)
                             .arg(m_corners.topRight ? 1 : 0)
                             .arg(m_corners.bottomLeft ? 1 : 0)
                             .arg(m_corners.bottomRight ? 1 : 0);
    for (const auto &layer : m_style) {
        result += QStringLiteral(":%1:%2:%3:%4")
                          .arg(layer.sigma, 0, 'g', 9)
                          .arg(layer.offset.x())
                          .arg(layer.offset.y())
                          .arg(layer.opacity, 0, 'g', 9);
    }
    if (!m_silhouette.isEmpty()) {
        result += QStringLiteral(":%1:%2,%3:%4,%5")
                          .arg(render::shapeKey(m_silhouette))
                          .arg(m_stretchBegin.x())
                          .arg(m_stretchBegin.y())
                          .arg(m_stretchEnd.x())
                          .arg(m_stretchEnd.y());
    }
    return result;
}

std::unique_ptr<render::Texture> ShadowGeometry::render(render::Renderer &renderer) const
{
    if (m_rasterSize.isEmpty())
        return nullptr;
    std::vector<render::ShadowLobe> lobes;
    for (const auto &layer : m_style) {
        if (layer.opacity > 0)
            lobes.push_back({layer.sigma * m_scale, layer.offset * m_scale, layer.opacity});
    }
    if (lobes.empty())
        return nullptr;

    const QRect box(QPoint(m_margins.left(), m_margins.top()) * m_scale, m_canonicalSize * m_scale);
    // A path has no shader: it is rasterized whole, then the stretched runs are
    // left out but for one sample. Black, so only its coverage is carried.
    QImage mask;
    if (!m_silhouette.isEmpty()) {
        const QImage full = render::rasterizeShape(
                m_silhouette, {double(m_scale), QPointF(), m_windowSize * m_scale});
        mask = QImage(box.size(), QImage::Format_RGBA8888_Premultiplied);
        mask.fill(Qt::transparent);
        const auto source = [&](int canonical, int begin, int end) {
            return canonical < (begin + 1) * m_scale || end <= begin
                    ? canonical
                    : canonical + (end - begin - 1) * m_scale;
        };
        for (int y = 0; y < box.height(); ++y) {
            const uchar *line = full.constScanLine(source(y, m_stretchBegin.y(), m_stretchEnd.y()));
            uchar *target = mask.scanLine(y);
            for (int x = 0; x < box.width(); ++x)
                target[x * 4 + 3] = line[source(x, m_stretchBegin.x(), m_stretchEnd.x()) * 4 + 3];
        }
    }

    return renderer.renderShadow(m_rasterSize, lobes, [&] {
        const render::Color black {0, 0, 0, 1};
        if (!mask.isNull()) {
            const auto texture = renderer.createTexture(mask.size(), mask.constBits(),
                                                        mask.bytesPerLine(),
                                                        render::PixelFormat::Rgba8888);
            if (texture)
                renderer.drawTexture(texture.get(), box, 1);
            return;
        }
        renderer.pushRoundedClip({QRectF(box), qreal(m_radius * m_scale)});
        renderer.fillRect(box, black);
        renderer.popRoundedClip();
        // A square corner fills the quarter of the box its arc would round.
        const QRectF bounds(box);
        const QSizeF quarter = bounds.size() / 2;
        if (!m_corners.topLeft)
            renderer.fillRectF({bounds.topLeft(), quarter}, black);
        if (!m_corners.topRight)
            renderer.fillRectF({QPointF(bounds.center().x(), bounds.top()), quarter}, black);
        if (!m_corners.bottomLeft)
            renderer.fillRectF({QPointF(bounds.left(), bounds.center().y()), quarter}, black);
        if (!m_corners.bottomRight)
            renderer.fillRectF({bounds.center(), quarter}, black);
    });
}

namespace {

render::Texture *shadowTexture(render::Renderer &renderer, const ShadowGeometry &geometry)
{
    const auto &textures = renderer.cachedTextureSet(geometry.key(), [&] {
        render::Renderer::TextureSet set;
        set.push_back(geometry.render(renderer));
        return set;
    });
    return textures.front().get();
}

// The unit square of a slice to `source`, in texture coordinates of `size`.
QTransform sliceMapping(const QRectF &source, const QSizeF &size)
{
    return QTransform(source.width() / size.width(), 0, 0, source.height() / size.height(),
                      source.x() / size.width(), source.y() / size.height());
}

} // namespace

void paintShadow(render::Renderer &renderer, const ShadowGeometry &geometry, const QPoint &position,
                 float opacity, const QMargins &bufferMargins)
{
    if (geometry.paintBounds().isEmpty() || opacity <= 0)
        return;
    render::Texture *image = shadowTexture(renderer, geometry);
    if (!image)
        return;
    // The shadow meets the window where its buffer lands, so the pixel along an
    // edge never gets only part of the shadow's darkest row.
    const QSize window = (geometry.paintBounds() - geometry.margins()).size();
    const QRectF body = renderer.pixelAligned(QRectF(position, window), bufferMargins);
    const auto place = [](int value, int extent, qreal begin, qreal end) {
        return value * 2 <= extent ? begin + value : end + (value - extent);
    };
    std::vector<render::Renderer::MappedRect> slices;
    for (size_t i = 0; i < 9; ++i) {
        const QRect source = geometry.sources()[i];
        const QRect target = geometry.targets()[i];
        // The middle is the window's own.
        if (i == 4 || source.isEmpty() || target.isEmpty())
            continue;
        const QPointF start(place(target.left(), window.width(), body.left(), body.right()),
                            place(target.top(), window.height(), body.top(), body.bottom()));
        const QPointF end(
                place(target.x() + target.width(), window.width(), body.left(), body.right()),
                place(target.y() + target.height(), window.height(), body.top(), body.bottom()));
        const QRectF placed(start, end);
        if (!renderer.drawsInto(placed.toAlignedRect()))
            continue;
        // A stretched cut lies in a constant run and any other joins slices
        // drawn at one scale, so filtering across it reads what one image would.
        slices.push_back({placed, sliceMapping(source, image->size())});
    }
    renderer.drawTextureMappedRects(image, slices, opacity);
}

void paintStretchedShadow(render::Renderer &renderer, const ShadowGeometry &geometry,
                          const QRect &window, float opacity)
{
    const QRect from = geometry.paintBounds() - geometry.margins();
    if (from.isEmpty() || window.isEmpty() || opacity <= 0)
        return;
    render::Texture *image = shadowTexture(renderer, geometry);
    if (!image)
        return;
    // The margins keep their size and only the span under the window scales,
    // so the shadow still meets the window's edges.
    const auto map = [](int value, int from, int to) {
        if (value <= 0)
            return value;
        if (value >= from)
            return value - from + to;
        return int(std::lround(double(value) * to / from));
    };
    // [begin, end) cut where the window's edges cross it.
    const auto pieces = [](int begin, int end, int extent) {
        std::vector<std::pair<int, int>> result;
        int start = begin;
        for (int cut : {0, extent}) {
            if (cut > start && cut < end) {
                result.push_back({start, cut});
                start = cut;
            }
        }
        result.push_back({start, end});
        return result;
    };
    std::vector<render::Renderer::MappedRect> slices;
    for (size_t i = 0; i < 9; ++i) {
        const QRect source = geometry.sources()[i];
        const QRect target = geometry.targets()[i];
        if (i == 4 || source.isEmpty() || target.isEmpty())
            continue;
        const double sx = double(source.width()) / target.width();
        const double sy = double(source.height()) / target.height();
        for (const auto &[top, bottom] :
             pieces(target.top(), target.y() + target.height(), from.height())) {
            for (const auto &[left, right] :
                 pieces(target.left(), target.x() + target.width(), from.width())) {
                const QPoint start(map(left, from.width(), window.width()),
                                   map(top, from.height(), window.height()));
                const QPoint end(map(right, from.width(), window.width()),
                                 map(bottom, from.height(), window.height()));
                const QRect placed =
                        QRect(start, QSize(end.x() - start.x(), end.y() - start.y()))
                                .translated(window.topLeft());
                if (placed.isEmpty() || !renderer.drawsInto(placed))
                    continue;
                const QRectF piece(source.x() + (left - target.x()) * sx,
                                   source.y() + (top - target.y()) * sy, (right - left) * sx,
                                   (bottom - top) * sy);
                slices.push_back({placed, sliceMapping(piece, image->size())});
            }
        }
    }
    renderer.drawTextureMappedRects(image, slices, opacity);
}

} // namespace cutefish::decoration
