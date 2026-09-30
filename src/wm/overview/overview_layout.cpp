// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/overview/overview_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace cutefish::wm::overview {

namespace {

// Windows grow by this share of their own size at a time, and by half as much
// again each time none can; they stop at half a pixel.
constexpr double kFirstGrowth = 1.0 / 64;
constexpr double kLastGrowthPixels = 0.5;
// A layout this much larger than another counts as larger; closer ones are a
// tie, which the one with fewer rows wins.
constexpr double kScaleTolerance = 1e-3;

struct Row {
    std::vector<size_t> windows;
    double width = 0;
    double height = 0;
};

struct Layout {
    std::vector<Row> rows;
    double scale = 0;
};

// A window as the rows lay it out: where its centre is, and the room it takes.
struct Cell {
    QPointF centre;
    QSizeF size;
};

// The windows taken top to bottom by their centres, cut into at most `count`
// rows of about equal width, each ordered left to right.
std::vector<Row> rowsOf(const std::vector<Cell> &windows, int count)
{
    std::vector<size_t> order(windows.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return windows[a].centre.y() < windows[b].centre.y();
    });
    double total = 0;
    for (const Cell &window : windows)
        total += window.size.width();
    const double share = total / count;

    std::vector<Row> rows(1);
    for (const size_t index : order) {
        const double width = windows[index].size.width();
        const Row &row = rows.back();
        // On to the next row once this one is nearer its share without the
        // window than with it.
        if (!row.windows.empty() && int(rows.size()) < count
            && row.width + width - share > share - row.width)
            rows.emplace_back();
        Row &target = rows.back();
        target.windows.push_back(index);
        target.width += width;
        target.height = std::max(target.height, windows[index].size.height());
    }
    for (Row &row : rows) {
        std::stable_sort(row.windows.begin(), row.windows.end(), [&](size_t a, size_t b) {
            return windows[a].centre.x() < windows[b].centre.x();
        });
    }
    return rows;
}

// The largest scale, never above 1, at which the rows fit `area` with `gap`
// between windows and between rows; 0 when they do not fit at all.
double fittingScale(const std::vector<Row> &rows, const QRectF &area, double gap)
{
    double scale = 1;
    double heights = 0;
    for (const Row &row : rows) {
        const double room = area.width() - double(row.windows.size() - 1) * gap;
        scale = std::min(scale, room / row.width);
        heights += row.height;
    }
    scale = std::min(scale, (area.height() - double(rows.size() - 1) * gap) / heights);
    return std::max(scale, 0.0);
}

// The space between two rectangles along whichever axis parts them; negative
// when they overlap.
double gapBetween(const QRectF &a, const QRectF &b)
{
    const double horizontal = std::max(b.left() - a.right(), a.left() - b.right());
    const double vertical = std::max(b.top() - a.bottom(), a.top() - b.bottom());
    return std::max(horizontal, vertical);
}

// Grows each rectangle on its own, at its window's shape and never past its
// window's size, into whatever room the others and the area leave it: the
// scale that made the fullest row fit leaves the rest smaller than they need
// to be. All grow at the same rate of their own size, so the large ones do not
// take all the room first.
void fillGaps(std::vector<QRectF> &rects, const std::vector<QRect> &windows, const QRectF &area,
              double gap)
{
    const QRectF inside = area.adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
    const auto fits = [&](const QRectF &candidate, size_t index) {
        if (!inside.contains(candidate))
            return false;
        for (size_t other = 0; other < rects.size(); ++other) {
            // Rounding leaves a rectangle kept at the gap a hair short of it.
            if (other != index && gapBetween(candidate, rects[other]) < gap - 1e-6)
                return false;
        }
        return true;
    };
    int widest = 0;
    for (const QRect &window : windows)
        widest = std::max(widest, window.width());
    // About its centre first, then away from each corner in turn.
    constexpr std::array<QPointF, 5> kAnchors = {QPointF(0.5, 0.5), QPointF(0, 0), QPointF(1, 0),
                                                 QPointF(0, 1), QPointF(1, 1)};
    for (double growth = kFirstGrowth; growth * widest >= kLastGrowthPixels; growth /= 2) {
        bool grew = true;
        while (grew) {
            grew = false;
            for (size_t index = 0; index < rects.size(); ++index) {
                const QRectF rect = rects[index];
                const QSize window = windows[index].size();
                if (rect.width() >= window.width())
                    continue;
                const double width =
                        std::min<double>(window.width(), rect.width() + window.width() * growth);
                const double height = width * window.height() / window.width();
                const double dw = width - rect.width();
                const double dh = height - rect.height();
                for (const QPointF &anchor : kAnchors) {
                    const QRectF candidate(rect.x() - dw * anchor.x(), rect.y() - dh * anchor.y(),
                                           width, height);
                    if (fits(candidate, index)) {
                        rects[index] = candidate;
                        grew = true;
                        break;
                    }
                }
            }
        }
    }
}

double snapped(double value, double deviceScale)
{
    return std::round(value * deviceScale) / deviceScale;
}

} // namespace

std::vector<QRectF> spreadLayout(const std::vector<QRect> &windows, const QRect &area, int gap,
                                 double deviceScale)
{
    std::vector<QRectF> result;
    if (windows.empty() || area.isEmpty())
        return result;
    deviceScale = deviceScale > 0 ? deviceScale : 1;
    // Snapping each edge may take a device pixel off a gap.
    const double required = gap + 1 / deviceScale;
    const QRectF bounds(area);
    std::vector<QRect> sizes;
    sizes.reserve(windows.size());
    double largest = 0;
    for (const QRect &window : windows) {
        sizes.emplace_back(window.topLeft(), window.size().expandedTo(QSize(1, 1)));
        largest = std::max(largest, double(sizes.back().width()) * sizes.back().height());
    }
    // A smaller window gets more room than its size, by the fourth root of how
    // much smaller its area is, so it is not shrunk as far as the largest.
    std::vector<Cell> cells;
    cells.reserve(sizes.size());
    for (const QRect &window : sizes) {
        const double boost = std::pow(largest / (double(window.width()) * window.height()), 0.25);
        cells.push_back({QRectF(window).center(), QSizeF(window.size()) * boost});
    }

    Layout best;
    for (int count = 1; count <= int(cells.size()); ++count) {
        std::vector<Row> rows = rowsOf(cells, count);
        // Fewer rows than asked for: a layout already tried.
        if (int(rows.size()) < count)
            continue;
        const double scale = fittingScale(rows, bounds, required);
        if (best.rows.empty() || scale > best.scale * (1 + kScaleTolerance))
            best = Layout {std::move(rows), scale};
    }

    // Each row centred, each window centred in its slot and never larger than
    // it is.
    std::vector<QRectF> placed(sizes.size());
    double height = double(best.rows.size() - 1) * required;
    for (const Row &row : best.rows)
        height += row.height * best.scale;
    double y = bounds.center().y() - height / 2;
    for (const Row &row : best.rows) {
        double x = bounds.center().x()
                - (row.width * best.scale + double(row.windows.size() - 1) * required) / 2;
        const double rowHeight = row.height * best.scale;
        for (const size_t index : row.windows) {
            const QSizeF cell = cells[index].size * best.scale;
            const QSizeF size = cell.width() > sizes[index].width() ? QSizeF(sizes[index].size())
                                                                    : cell;
            placed[index] = QRectF(QPointF(x + (cell.width() - size.width()) / 2,
                                           y + (rowHeight - size.height()) / 2),
                                   size);
            x += cell.width() + required;
        }
        y += rowHeight + required;
    }
    fillGaps(placed, sizes, bounds, required);

    // Grown unevenly, the arrangement is centred again.
    QRectF extent;
    for (const QRectF &rect : placed)
        extent |= rect;
    const QPointF shift = bounds.center() - extent.center();
    result.reserve(placed.size());
    for (const QRectF &rect : placed) {
        const QRectF moved = rect.translated(shift);
        const QPointF a(snapped(moved.left(), deviceScale), snapped(moved.top(), deviceScale));
        const QPointF b(snapped(moved.right(), deviceScale), snapped(moved.bottom(), deviceScale));
        result.emplace_back(a, b);
    }
    return result;
}

} // namespace cutefish::wm::overview
