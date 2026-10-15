// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "decoration/raster.h"

#include <algorithm>
#include <cmath>

#include "render/rounded_shape.h"

namespace cutefish::decoration {

namespace {

constexpr int kSamples = 4;

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

bool insideRoundedRect(const base::RectF& rect, double radius, const Corners& corners, double x, double y,
                       bool continuous = false)
{
    if (x < rect.left() || x > rect.right() || y < rect.top() || y > rect.bottom())
        return false;
    if (radius <= 0.0)
        return true;

    const double rx = continuous ? render::continuousCornerExtent(radius, rect.width() / 2)
                               : std::min(radius, std::min(rect.width(), rect.height()) / 2);
    const double ry = continuous ? render::continuousCornerExtent(radius, rect.height() / 2) : rx;
    if (rx <= 0 || ry <= 0)
        return true;
    const double left = rect.left() + rx;
    const double right = rect.right() - rx;
    const double top = rect.top() + ry;
    const double bottom = rect.bottom() - ry;

    const bool rounded = (x < left && y < top && corners.topLeft)
            || (x > right && y < top && corners.topRight)
            || (x < left && y > bottom && corners.bottomLeft)
            || (x > right && y > bottom && corners.bottomRight);
    if (!rounded)
        return true;

    if (continuous)
        return render::RoundedShape {rect, radius}.contains({x, y});

    const double dx = std::max(std::abs(x - rect.center().x()) - rect.width() / 2 + rx, double(0));
    const double dy = std::max(std::abs(y - rect.center().y()) - rect.height() / 2 + ry, double(0));
    return dx * dx + dy * dy <= rx * rx;
}

double distanceToSegment(const base::PointF& point, const base::PointF& from, const base::PointF& to)
{
    const base::PointF direction = to - from;
    const double lengthSquared =
        direction.x() * direction.x() + direction.y() * direction.y();
    double t = 0.0;
    if (lengthSquared > 0.0) {
        t = ((point.x() - from.x()) * direction.x() + (point.y() - from.y()) * direction.y())
            / lengthSquared;
        t = std::clamp(t, 0.0, 1.0);
    }
    const base::PointF closest = from + direction * t;
    return std::hypot(point.x() - closest.x(), point.y() - closest.y());
}

// Runs `test` over a 4x4 grid inside pixel (x, y) and returns the fraction that
// passed.
template<typename F>
float coverage(int x, int y, F&& test)
{
    int hits = 0;
    for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
            const double px = x + (sx + 0.5) / kSamples;
            const double py = y + (sy + 0.5) / kSamples;
            if (test(px, py))
                ++hits;
        }
    }
    return static_cast<float>(hits) / (kSamples * kSamples);
}

// The pixels a shape bounded by `rect` can touch, the far edges' included.
base::Rect pixelBounds(const Image& image, const base::RectF& rect, double margin = 1.0)
{
    const base::Rect bounds = base::Rect::fromEdges(
        static_cast<int>(std::floor(rect.left() - margin)),
        static_cast<int>(std::floor(rect.top() - margin)),
        static_cast<int>(std::ceil(rect.right() + margin)) + 1,
        static_cast<int>(std::ceil(rect.bottom() + margin)) + 1);
    return bounds.intersected(base::Rect(image.size()));
}

template<typename F>
void rasterize(Image& image, const base::RectF& area, F&& test, const render::Color& color)
{
    const base::Rect bounds = pixelBounds(image, area);
    for (int y = bounds.top(); y < bounds.bottom(); ++y) {
        for (int x = bounds.left(); x < bounds.right(); ++x) {
            const float alpha = coverage(x, y, test);
            if (alpha > 0.0f)
                image.blend(x, y, color, alpha);
        }
    }
}

// Whole pixels no corner reaches, which every sample of a fill would hit.
// Skipping their supersampling keeps a window-sized shadow mask cheap.
struct SolidPixels {
    SolidPixels(const base::RectF& rect, double radius, bool continuous)
        : rect(rect)
    {
        if (radius > 0.0) {
            rx = continuous ? render::continuousCornerExtent(radius, rect.width() / 2)
                            : std::min(radius, std::min(rect.width(), rect.height()) / 2);
            ry = continuous ? render::continuousCornerExtent(radius, rect.height() / 2) : rx;
        }
    }

    bool contains(int x, int y) const
    {
        if (x < rect.left() || x + 1 > rect.right() || y < rect.top() || y + 1 > rect.bottom())
            return false;
        return (x >= rect.left() + rx && x + 1 <= rect.right() - rx)
            || (y >= rect.top() + ry && y + 1 <= rect.bottom() - ry);
    }

    base::RectF rect;
    double rx = 0.0;
    double ry = 0.0;
};

template<typename F>
void fill(Image& image, const base::RectF& area, const SolidPixels& solid, F&& test,
          const render::Color& color)
{
    const base::Rect bounds = pixelBounds(image, area);
    for (int y = bounds.top(); y < bounds.bottom(); ++y) {
        for (int x = bounds.left(); x < bounds.right(); ++x) {
            const float alpha = solid.contains(x, y) ? 1.0f : coverage(x, y, test);
            if (alpha > 0.0f)
                image.blend(x, y, color, alpha);
        }
    }
}

} // namespace

Image::Image(const base::Size& size)
    : m_size(size)
    , m_pixels(static_cast<size_t>(std::max(0, m_size.width()))
                   * static_cast<size_t>(std::max(0, m_size.height())) * 4,
               0)
{
}

void Image::blend(int x, int y, const render::Color& color, float coverage)
{
    if (x < 0 || y < 0 || x >= m_size.width() || y >= m_size.height())
        return;

    const float alpha = color.a * std::clamp(coverage, 0.0f, 1.0f);
    if (alpha <= 0.0f)
        return;

    uint8_t* pixel = &m_pixels[(static_cast<size_t>(y) * m_size.width() + x) * 4];
    const float inverse = 1.0f - alpha;
    // Premultiplied source over premultiplied destination.
    pixel[0] = toByte(color.r * alpha + pixel[0] / 255.0f * inverse);
    pixel[1] = toByte(color.g * alpha + pixel[1] / 255.0f * inverse);
    pixel[2] = toByte(color.b * alpha + pixel[2] / 255.0f * inverse);
    pixel[3] = toByte(alpha + pixel[3] / 255.0f * inverse);
}

void Image::erase(int x, int y, float coverage)
{
    if (x < 0 || y < 0 || x >= m_size.width() || y >= m_size.height())
        return;
    const float keep = 1.0f - std::clamp(coverage, 0.0f, 1.0f);
    uint8_t* pixel = &m_pixels[(static_cast<size_t>(y) * m_size.width() + x) * 4];
    for (int channel = 0; channel < 4; ++channel)
        pixel[channel] = toByte(pixel[channel] / 255.0f * keep);
}

void fillRoundedRect(Image& image, const base::RectF& rect, double radius, const render::Color& color,
                     const Corners& corners)
{
    fill(
        image, rect, SolidPixels(rect, radius, false),
        [&](double x, double y) {
            return insideRoundedRect(rect, radius, corners, x, y);
        },
        color);
}

void fillContinuousRoundedRect(Image& image, const base::RectF& rect, double radius,
                               const render::Color& color, const Corners& corners)
{
    fill(
        image, rect, SolidPixels(rect, radius, true),
        [&](double x, double y) {
            return insideRoundedRect(rect, radius, corners, x, y, true);
        },
        color);
}

void strokeRoundedRect(Image& image, const base::RectF& rect, double radius, double width,
                       const render::Color& color, const Corners& corners)
{
    // A stroke wider than half the rect leaves no inside to cut out.
    const bool hollow = rect.width() >= 2 * width && rect.height() >= 2 * width;
    const base::RectF inner = hollow ? rect.adjusted(width, width, -width, -width) : base::RectF();
    const double innerRadius = std::max(0.0, radius - width);
    rasterize(
        image, rect,
        [&](double x, double y) {
            return insideRoundedRect(rect, radius, corners, x, y)
                && !(hollow && insideRoundedRect(inner, innerRadius, corners, x, y));
        },
        color);
}

void eraseRoundedRect(Image& image, const base::RectF& rect, double radius)
{
    const base::Rect bounds = pixelBounds(image, rect);
    const Corners corners = Corners::all();
    for (int y = bounds.top(); y < bounds.bottom(); ++y) {
        for (int x = bounds.left(); x < bounds.right(); ++x) {
            const float alpha = coverage(x, y, [&](double px, double py) {
                return insideRoundedRect(rect, radius, corners, px, py);
            });
            if (alpha > 0.0f)
                image.erase(x, y, alpha);
        }
    }
}

void strokeLine(Image& image, const base::PointF& from, const base::PointF& to, double width,
                const render::Color& color)
{
    const double half = width / 2.0;
    const base::RectF area = base::RectF::fromEdges(
        std::min(from.x(), to.x()) - half, std::min(from.y(), to.y()) - half,
        std::max(from.x(), to.x()) + half, std::max(from.y(), to.y()) + half);
    rasterize(
        image, area,
        [&](double x, double y) {
            return distanceToSegment(base::PointF(x, y), from, to) <= half;
        },
        color);
}

} // namespace cutefish::decoration
