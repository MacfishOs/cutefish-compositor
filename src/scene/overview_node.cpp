// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "scene/overview_node.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <QColor>
#include <QPainter>
#include <QPen>

#include "scene/window_animation.h"

namespace cutefish::scene {

namespace {

constexpr render::Color kBlack {0, 0, 0, 1};
constexpr render::Color kWhite {1, 1, 1, 1};
constexpr render::Color kRingColor {1, 1, 1, 0.85f};
constexpr render::Color kSelectionColor {1, 1, 1, 0.55f};
// The title's plate is white glass: a blur of the window under it, lit.
constexpr render::Color kTitlePlateColor {1, 1, 1, 0.72f};
constexpr render::Color kTitleColor {0, 0, 0, 0.85f};
constexpr render::Color kClosePlateColor {0.16f, 0.16f, 0.16f, 0.92f};
constexpr render::Color kButtonColor {1, 1, 1, 0.16f};
constexpr render::Color kButtonHighlightColor {1, 1, 1, 0.3f};
constexpr render::Color kPreviewOutlineColor {1, 1, 1, 0.22f};

constexpr int kTitlePixelSize = 13;
constexpr int kTitleMaxWidth = 360;
constexpr double kTitlePaddingX = 12;
constexpr double kTitlePaddingY = 6;
constexpr double kTitleRadius = 6;
constexpr double kRingWidth = 2;
constexpr double kRingOffset = 3;
constexpr double kLabelGap = 6;
constexpr double kPlusSize = 14;
// Shadows reach this far outside a window; a window this far off an output
// draws nothing there.
constexpr int kShadowMargin = 64;
constexpr size_t kCacheLimit = 32;

double snap(double value, double scale)
{
    return scale > 0 ? std::round(value * scale) / scale : value;
}

// Every edge on a device pixel, so a window at rest is as sharp as it is on
// the desk and one in motion does not shimmer between two pixels.
QRectF snapped(const QRectF &rect, double scale)
{
    return QRectF(QPointF(snap(rect.left(), scale), snap(rect.top(), scale)),
                  QPointF(snap(rect.right(), scale), snap(rect.bottom(), scale)));
}

QRectF lerp(const QRectF &a, const QRectF &b, double t)
{
    return QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                  a.width() + (b.width() - a.width()) * t,
                  a.height() + (b.height() - a.height()) * t);
}

// `rect` moved and scaled as `from` is onto `to`.
QRectF mapped(const QRectF &rect, const QRectF &from, const QRectF &to)
{
    if (from.isEmpty())
        return to;
    const double sx = to.width() / from.width();
    const double sy = to.height() / from.height();
    return QRectF(to.x() + (rect.x() - from.x()) * sx, to.y() + (rect.y() - from.y()) * sy,
                  rect.width() * sx, rect.height() * sy);
}

render::Color faded(render::Color color, float opacity)
{
    color.a *= opacity;
    return color;
}

QColor toQColor(const render::Color &color)
{
    return QColor::fromRgbF(color.r, color.g, color.b, color.a);
}

QString colorKey(const render::Color &color)
{
    return QStringLiteral("%1,%2,%3,%4").arg(color.r).arg(color.g).arg(color.b).arg(color.a);
}

std::function<qint64()> steadyClock()
{
    return [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
    };
}

double frameNow(const render::Renderer &renderer, const std::function<qint64()> &clock)
{
    return renderer.frameTime >= 0 ? renderer.frameTime : double(clock());
}

QRect viewportOf(const render::Renderer &renderer, const Node &node)
{
    const QRect viewport = renderer.outputContext().logicalViewport;
    return viewport.isEmpty() ? node.absoluteGeometry() : viewport;
}

std::pair<QString, int> previewKey(const OverviewDeskBarNode::Bar &bar, int desk)
{
    return {QStringLiteral("%1,%2,%3,%4")
                    .arg(bar.output.x())
                    .arg(bar.output.y())
                    .arg(bar.output.width())
                    .arg(bar.output.height()),
            desk};
}

void fillRounded(render::Renderer &renderer, const QRectF &rect, qreal radius,
                 const render::Color &color)
{
    renderer.pushRoundedClip(render::RoundedShape {rect, radius});
    renderer.fillRectF(rect, color);
    renderer.popRoundedClip();
}

} // namespace

render::Texture *OverviewChromeCache::find(const QString &key, QSizeF *size)
{
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->key != key)
            continue;
        m_entries.splice(m_entries.begin(), m_entries, it);
        if (size)
            *size = m_entries.front().size;
        return m_entries.front().texture.get();
    }
    return nullptr;
}

render::Texture *OverviewChromeCache::insert(render::Renderer &renderer, const QString &key,
                                             const QImage &image, const QSizeF &size)
{
    auto texture = renderer.createTexture(image.size(), image.constBits(), image.bytesPerLine(),
                                          render::PixelFormat::Rgba8888);
    if (!texture)
        return nullptr;
    m_entries.push_front({key, std::move(texture), size});
    while (m_entries.size() > kCacheLimit)
        m_entries.pop_back();
    return m_entries.front().texture.get();
}

render::Texture *OverviewChromeCache::label(render::Renderer &renderer,
                                            const OverviewTextPainter &painter,
                                            const QString &text, int pixelSize,
                                            const render::Color &color, int maxWidth,
                                            QSizeF *size)
{
    if (!painter || text.isEmpty())
        return nullptr;
    const double density = renderer.rasterScale();
    const QString key = QStringLiteral("label|%1|%2|%3|%4|%5")
                                .arg(text)
                                .arg(pixelSize)
                                .arg(colorKey(color))
                                .arg(maxWidth)
                                .arg(density);
    if (render::Texture *texture = find(key, size))
        return texture;
    QImage image = painter(text, pixelSize, color, density, maxWidth);
    if (image.isNull())
        return nullptr;
    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QSizeF logical(image.width() / density, image.height() / density);
    if (size)
        *size = logical;
    return insert(renderer, key, image, logical);
}

render::Texture *OverviewChromeCache::glyph(render::Renderer &renderer, Glyph glyph,
                                            const QSizeF &size, const render::Color &color)
{
    const double density = renderer.rasterScale();
    const QString key = QStringLiteral("glyph|%1|%2x%3|%4|%5")
                                .arg(int(glyph))
                                .arg(size.width())
                                .arg(size.height())
                                .arg(colorKey(color))
                                .arg(density);
    if (render::Texture *texture = find(key, nullptr))
        return texture;
    const QSize pixels(std::max(1, int(std::ceil(size.width() * density))),
                       std::max(1, int(std::ceil(size.height() * density))));
    QImage image(pixels, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds(0, 0, pixels.width(), pixels.height());
        QPen pen(toQColor(color), std::max(1.0, 1.6 * density), Qt::SolidLine, Qt::RoundCap);
        switch (glyph) {
        case Glyph::Close: {
            painter.setPen(Qt::NoPen);
            painter.setBrush(toQColor(kClosePlateColor));
            painter.drawEllipse(bounds);
            painter.setPen(pen);
            const double inset = bounds.width() * 0.34;
            const QRectF cross = bounds.adjusted(inset, inset, -inset, -inset);
            painter.drawLine(cross.topLeft(), cross.bottomRight());
            painter.drawLine(cross.topRight(), cross.bottomLeft());
            break;
        }
        case Glyph::Plus: {
            painter.setPen(pen);
            const QPointF centre = bounds.center();
            const double half = std::min(bounds.width(), bounds.height()) / 2 - pen.widthF();
            painter.drawLine(centre - QPointF(half, 0), centre + QPointF(half, 0));
            painter.drawLine(centre - QPointF(0, half), centre + QPointF(0, half));
            break;
        }
        }
    }
    return insert(renderer, key, image, size);
}

OverviewNode::OverviewNode(std::function<qint64()> clock)
    : Node(QStringLiteral("overview"))
    , m_now(clock ? std::move(clock) : steadyClock())
{
}

void OverviewNode::setBackdrop(std::vector<const Node *> nodes)
{
    m_backdrop = std::move(nodes);
    notifyChanged();
}

void OverviewNode::setItems(std::vector<Item> items, bool animate)
{
    const double now = double(m_now());
    std::vector<QRectF> from;
    from.reserve(items.size());
    if (animate) {
        // Each window already shown sets off from where it is headed now.
        const double t = layoutProgress(now);
        std::map<const WindowAnimation *, QRectF> current;
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i].windows.empty())
                continue;
            current[m_items[i].windows.front()] = int(i) == m_dragged
                    ? m_dragRect
                    : lerp(m_layoutFrom[i], m_items[i].to, t);
        }
        for (const Item &item : items) {
            const auto it = item.windows.empty() ? current.end()
                                                 : current.find(item.windows.front());
            from.push_back(it == current.end() ? item.to : it->second);
        }
        m_layout = motion::Tween(now, 0, 1, kLayoutDurationMs);
    } else {
        for (const Item &item : items)
            from.push_back(item.to);
        m_layout.reset();
    }
    m_items = std::move(items);
    m_layoutFrom = std::move(from);
    m_dragged = -1;
    m_chrome = {};
    notifyChanged();
}

void OverviewNode::setDragged(int index, const QRectF &rect)
{
    m_dragged = index >= 0 && index < int(m_items.size()) ? index : -1;
    m_dragRect = rect;
    notifyChanged();
}

void OverviewNode::setChrome(const Chrome &chrome)
{
    if (chrome.hovered == m_chrome.hovered && chrome.selected == m_chrome.selected)
        return;
    m_chrome = chrome;
    notifyChanged();
}

double OverviewNode::progress(double timeMs) const
{
    switch (m_motion) {
    case Motion::Still:
        return m_position;
    case Motion::Tracking:
        if (m_coast && timeMs > m_coastStart)
            return m_coast->value(timeMs);
        // The resampled fingers can be predicted past an end.
        return std::clamp(m_fingers.empty()
                                  ? m_position
                                  : m_fingers.position(timeMs
                                                       - motion::FingerTrack::kResampleLatencyMs),
                          0.0, 1.0);
    case Motion::Tween:
        return m_tween.value(timeMs);
    }
    return m_position;
}

double OverviewNode::target() const
{
    switch (m_motion) {
    case Motion::Still:
        return m_position;
    case Motion::Tracking:
        return m_position >= 0.5 ? 1 : 0;
    case Motion::Tween:
        return m_tween.target();
    }
    return m_position;
}

motion::Tween OverviewNode::tweenTo(double now, double from, double target,
                                    double velocity) const
{
    // Ease-in-out with its start bent to the fingers' speed: a slow release
    // speeds up gently, a fast one only slows down. ACCEL_20_DECEL_100 bent the
    // same way surges to four times the average speed right after the release.
    // Less of the way takes less time, though more than its share, so a short
    // way back is not a snap.
    const bool fromRest = velocity * (target - from) <= 0;
    const double whole = fromRest ? kRestDurationMs : kShowDurationMs;
    const double duration = whole * std::sqrt(std::min(std::abs(target - from), 1.0));
    return motion::Tween(now, from, target, duration, velocity, motion::kEaseInOut);
}

void OverviewNode::settle(double now, double from, double target, double velocity)
{
    m_tween = tweenTo(now, from, target, velocity);
    m_motion = Motion::Tween;
    m_fingers.clear();
    m_coast.reset();
    m_shownAt = -1;
    notifyChanged();
}

void OverviewNode::animateTo(double target)
{
    const double now = double(m_now());
    if (m_motion == Motion::Still && m_position == target)
        return;
    const double velocity = m_motion == Motion::Tween ? m_tween.velocity(now) : 0;
    settle(now, progress(now), target, velocity);
}

void OverviewNode::jumpTo(double value)
{
    m_motion = Motion::Still;
    m_position = value;
    m_fingers.clear();
    m_coast.reset();
    m_shownAt = value >= 1 ? double(m_now()) : -1;
    notifyChanged();
}

void OverviewNode::startTracking()
{
    m_position = progress(double(m_now()));
    m_motion = Motion::Tracking;
    m_fingers.clear();
    m_coast.reset();
    m_shownAt = -1;
    notifyChanged();
}

void OverviewNode::updateTracking(double delta, double timeMs)
{
    // The fingers catch the progress where it is shown: moving on its own,
    // or coasting since they paused rather than lifted.
    const bool coasted = m_motion == Motion::Tracking && m_coast
            && timeMs > m_coastStart - motion::FingerTrack::kResampleLatencyMs
                            + motion::FingerTrack::kResampleMaxPredictionMs;
    if (m_motion != Motion::Tracking || coasted)
        startTracking();
    // Stops dead at either end, so the fingers turning back move it at once.
    m_position = std::clamp(m_position + delta, 0.0, 1.0);
    m_fingers.add(timeMs, m_position);
    updateCoast();
    notifyChanged();
}

void OverviewNode::updateCoast()
{
    m_coast.reset();
    // Fingers that lift leave the pad at speed; ones that stop to hold slow
    // down first, which the last events show before the fit does.
    const std::optional<double> velocity = m_fingers.recentVelocity();
    if (!velocity || std::abs(*velocity) < kFlickVelocity)
        return;
    const double from = m_fingers.predictionEnd();
    const double position = std::clamp(m_fingers.position(from), 0.0, 1.0);
    m_coastStart = from + motion::FingerTrack::kResampleLatencyMs;
    m_coast = tweenTo(m_coastStart, position, *velocity > 0 ? 1 : 0, *velocity);
}

double OverviewNode::endTracking(double timeMs)
{
    if (m_motion != Motion::Tracking)
        return target();
    const double now = double(m_now());
    if (m_coast) {
        // Already on its way since the fingers stopped reporting.
        m_tween = *m_coast;
        m_motion = Motion::Tween;
        m_fingers.clear();
        m_coast.reset();
        notifyChanged();
        return m_tween.target();
    }
    double velocity = m_fingers.releaseVelocity(timeMs);
    const double current = progress(now);
    // Flung against an end it already stands at.
    if ((current <= 0 && velocity < 0) || (current >= 1 && velocity > 0))
        velocity = 0;
    double target = current >= 0.5 ? 1 : 0;
    if (velocity >= kFlickVelocity)
        target = 1;
    else if (velocity <= -kFlickVelocity)
        target = 0;
    settle(now, current, target, velocity);
    return target;
}

bool OverviewNode::backdropCovers(const QRect &viewport) const
{
    // The first thing drawn that is opaque over the whole output; anything
    // drawn before it ends up under it.
    const std::function<bool(const Node &)> covers = [&](const Node &node) {
        if (!node.isVisible())
            return false;
        if (node.content() == Content::Opaque && node.absoluteGeometry().contains(viewport))
            return true;
        for (const Node *child : node.children()) {
            if (covers(*child))
                return true;
        }
        return false;
    };
    return std::any_of(m_backdrop.begin(), m_backdrop.end(), [&](const Node *node) {
        return covers(*node);
    });
}

double OverviewNode::layoutProgress(double timeMs) const
{
    return m_layout ? m_layout->value(timeMs) : 1;
}

QRectF OverviewNode::presentedRect(int index, double progress, double timeMs) const
{
    const Item &item = m_items[index];
    const QRectF to = index == m_dragged
            ? m_dragRect
            : lerp(m_layoutFrom[index], item.to, layoutProgress(timeMs));
    if (item.windows.empty() || index == m_dragged)
        return to;
    return lerp(item.windows.front()->absoluteGeometry(), to, progress);
}

float OverviewNode::chromeOpacity(double timeMs) const
{
    if (m_motion != Motion::Still || m_position < 1 || m_shownAt < 0)
        return 0;
    const double fade = kChromeFadeMs * motion::durationScale();
    return float(std::clamp((timeMs - m_shownAt) / fade, 0.0, 1.0));
}

void OverviewNode::landed(double now) const
{
    m_motion = Motion::Still;
    m_position = m_tween.target();
    m_shownAt = m_position >= 1 ? now : -1;
    if (m_finished)
        m_finished(m_position);
}

void OverviewNode::render(render::Renderer &renderer) const
{
    if (!isActive() || !isVisible())
        return;
    const double now = frameNow(renderer, m_now);
    const QRect viewport = viewportOf(renderer, *this);
    const double progress = this->progress(now);

    // The desktop is drawn darkened rather than under a dim pass, and one
    // that covers the output opaquely is darkened in place, with no clear and
    // no blending under it: full-output passes an iGPU can ill afford.
    const float dim = kDimOpacity * float(std::clamp(progress, 0.0, 1.0));
    const bool inPlace = !renderer.capturing && backdropCovers(viewport);
    if (!inPlace)
        renderer.fillRect(viewport, kBlack);
    const float opacity = renderer.opacityScale;
    const bool overwrite = renderer.overwriteOpaque;
    renderer.opacityScale = opacity * (1 - dim);
    renderer.overwriteOpaque = inPlace;
    for (const Node *node : m_backdrop) {
        // In place, drawn live as backdropCovers() saw it: a window's snapshot
        // is padded with transparent texels and would blend over the last frame.
        if (inPlace)
            node->Node::render(renderer);
        else
            node->render(renderer);
    }
    renderer.overwriteOpaque = overwrite;
    renderer.opacityScale = opacity;

    const int raised = m_raised < int(m_items.size()) && m_raised != m_dragged ? m_raised : -1;
    for (int index = 0; index < int(m_items.size()); ++index) {
        if (index != m_dragged && index != raised)
            renderItem(renderer, index, progress, now);
    }
    if (raised >= 0)
        renderItem(renderer, raised, progress, now);

    renderChrome(renderer, viewport, now);

    if (renderer.capturing)
        return;
    switch (m_motion) {
    case Motion::Still:
        break;
    case Motion::Tracking: {
        // Until the resampled position catches up with the newest event, or
        // for as long as it coasts.
        const bool coasting = m_coast && !m_coast->isFinished(now);
        if (coasting || m_fingers.isCatchingUp(now))
            notifyChanged();
        break;
    }
    case Motion::Tween:
        if (m_tween.isFinished(now))
            landed(now);
        // The landing frame, or the next one of the motion.
        notifyChanged();
        break;
    }
    if (m_layout && !m_layout->isFinished(now))
        notifyChanged();
    if (const float chrome = chromeOpacity(now); chrome > 0 && chrome < 1)
        notifyChanged();
}

void OverviewNode::renderItem(render::Renderer &renderer, int index, double progress,
                              double now) const
{
    const Item &item = m_items[index];
    if (item.windows.empty())
        return;
    QRectF rect = presentedRect(index, progress, now);
    if (progress > 0)
        rect = snapped(rect, renderer.rasterScale());
    const QRect reach = viewportOf(renderer, *this)
                                .adjusted(-kShadowMargin, -kShadowMargin, kShadowMargin,
                                          kShadowMargin);
    if (!rect.intersects(reach))
        return;
    const double uniform = std::clamp(progress, 0.0, 1.0);
    const bool moving = !isSettled() || (m_layout && !m_layout->isFinished(now));
    const QRectF from = item.windows.front()->absoluteGeometry();
    for (const WindowAnimation *window : item.windows) {
        const QRectF bounds = mapped(window->absoluteGeometry(), from, rect);
        const double scale = window->absoluteGeometry().isEmpty()
                ? 1
                : bounds.width() / window->absoluteGeometry().width();
        const WindowAnimation::Presentation presentation {bounds, 1, uniform,
                                                          m_frame.radius * scale};
        const render::RoundedShape body = window->presentedBody(presentation);
        if (m_frame.shadow)
            m_frame.shadow(renderer, *window, body, uniform, 1, moving);
        window->renderPresented(renderer, presentation);
        if (m_frame.outline && uniform > 0)
            m_frame.outline(renderer, body, float(uniform));
    }
}

void OverviewNode::setRaised(int index)
{
    if (m_raised == index)
        return;
    m_raised = index;
    notifyChanged();
}

void OverviewNode::renderDragged(render::Renderer &renderer) const
{
    if (!isActive() || !isVisible() || m_dragged < 0 || m_dragged >= int(m_items.size()))
        return;
    const double now = frameNow(renderer, m_now);
    renderItem(renderer, m_dragged, progress(now), now);
}

render::RoundedShape OverviewNode::restingBody(int index, const QRectF &rect) const
{
    const Item &item = m_items[index];
    if (item.windows.empty())
        return render::RoundedShape {rect, m_frame.radius};
    const WindowAnimation *window = item.windows.front();
    const double scale = window->absoluteGeometry().isEmpty()
            ? 1
            : rect.width() / window->absoluteGeometry().width();
    return window->presentedBody({rect, 1, 1, m_frame.radius * scale});
}

void OverviewNode::renderChrome(render::Renderer &renderer, const QRect &viewport,
                                double now) const
{
    const float opacity = chromeOpacity(now);
    if (opacity <= 0 || m_dragged >= 0)
        return;
    const double scale = renderer.rasterScale();
    const auto rectOf = [&](int index) {
        return snapped(presentedRect(index, 1, now), scale);
    };
    // Around the body, its corners the body's grown by the gap between them.
    const auto ring = [&](int index, const render::Color &color) {
        render::RoundedShape shape = restingBody(index, rectOf(index));
        shape.bounds.adjust(-kRingOffset, -kRingOffset, kRingOffset, kRingOffset);
        shape.radius += kRingOffset;
        renderer.drawRoundedOutline(shape, faded(color, opacity), kRingWidth * scale);
    };
    const int count = int(m_items.size());
    const int hovered = m_chrome.hovered < count ? m_chrome.hovered : -1;
    const int selected = m_chrome.selected < count ? m_chrome.selected : -1;
    if (selected >= 0 && selected != hovered)
        ring(selected, kSelectionColor);
    if (hovered >= 0)
        ring(hovered, kRingColor);

    const int titled = hovered >= 0 ? hovered : selected;
    if (titled >= 0 && viewport.intersects(rectOf(titled).toAlignedRect())) {
        const QRectF rect = rectOf(titled);
        // In the middle of the window, no wider than it leaves room for.
        const int maxWidth = std::min(kTitleMaxWidth,
                                      int(rect.width() - 4 * kTitlePaddingX));
        QSizeF size;
        render::Texture *label = maxWidth > 0
                ? m_chromeCache.label(renderer, m_textPainter, m_items[titled].title,
                                      kTitlePixelSize, kTitleColor, maxWidth, &size)
                : nullptr;
        if (label) {
            QRectF plate(0, 0, size.width() + 2 * kTitlePaddingX,
                         size.height() + 2 * kTitlePaddingY);
            plate.moveCenter(rect.center());
            plate = snapped(plate, scale);
            const render::RoundedShape shape {plate, kTitleRadius};
            render::GlassParams blur;
            blur.opacity = opacity;
            renderer.pushRoundedClip(shape);
            renderer.drawBackgroundBlur(plate.toAlignedRect(), qRound(shape.radius),
                                        plate.toAlignedRect(), blur);
            renderer.fillRectF(plate, faded(kTitlePlateColor, opacity));
            renderer.popRoundedClip();
            QRectF text(QPointF(), size);
            text.moveCenter(plate.center());
            renderer.drawTextureF(label, snapped(text, scale), opacity);
        }
    }
}

void OverviewNode::releaseRenderResources()
{
    m_chromeCache.clear();
    Node::releaseRenderResources();
}

OverviewDeskBarNode::OverviewDeskBarNode(const OverviewNode *overview)
    : Node(QStringLiteral("overview-desk-bar"))
    , m_overview(overview)
{
}

void OverviewDeskBarNode::setBars(std::vector<Bar> bars)
{
    m_bars = std::move(bars);
    notifyChanged();
}

void OverviewDeskBarNode::setContent(std::vector<const Node *> backdrop,
                                     std::vector<DeskContent> desks)
{
    m_backdrop = std::move(backdrop);
    m_desks = std::move(desks);
    invalidatePreviews();
}

void OverviewDeskBarNode::invalidatePreviews()
{
    for (auto &[key, preview] : m_previews)
        preview.stale = true;
    notifyChanged();
}

QRectF OverviewDeskBarNode::closeButtonRect(const QRectF &preview)
{
    return QRectF(preview.right() - kCloseButtonMargin - kCloseButtonSize,
                  preview.top() + kCloseButtonMargin, kCloseButtonSize, kCloseButtonSize);
}

double OverviewDeskBarNode::slideOffset(const Bar &bar, double progress)
{
    // Out of sight above the output's top edge at 0.
    return -(1 - std::clamp(progress, 0.0, 1.0)) * (bar.bounds.bottom() - bar.output.top());
}

void OverviewDeskBarNode::updatePreview(render::Renderer &renderer, const Bar &bar, int desk,
                                        double now) const
{
    if (renderer.capturing)
        return;
    const QRectF bounds = bar.desks[desk].bounds;
    Preview &preview = m_previews[previewKey(bar, desk)];
    const bool live = bar.desks[desk].active && now - preview.drawnAt >= kPreviewRefreshMs;
    if (preview.texture && !preview.stale && !live && preview.bounds == bounds
        && preview.scale == renderer.rasterScale())
        return;
    // One a frame, so the first frames of a swipe are not spent on all of
    // them; the rest show what they last did, or nothing, one frame longer.
    if (m_previewsLeft <= 0) {
        notifyChanged();
        return;
    }
    --m_previewsLeft;
    const QRectF output(bar.output);
    const QRect target = bounds.toAlignedRect();
    const bool drawn = renderer.renderToTexture(preview.texture, target, [&] {
        renderer.fillRect(target, kBlack);
        const bool blurSuppressed = renderer.suppressBackgroundBlur;
        renderer.suppressBackgroundBlur = true;
        const auto place = [&](const Node *node) {
            const auto *window = dynamic_cast<const WindowAnimation *>(node);
            const QRectF geometry = node->absoluteGeometry();
            if (window && geometry.intersects(output))
                window->renderPresented(renderer, {mapped(geometry, output, bounds)});
        };
        for (const Node *node : m_backdrop)
            place(node);
        if (desk < int(m_desks.size())) {
            for (const WindowAnimation *window : m_desks[desk].windows)
                place(window);
        }
        renderer.suppressBackgroundBlur = blurSuppressed;
    });
    if (!drawn)
        preview.texture.reset();
    preview.bounds = bounds;
    preview.scale = renderer.rasterScale();
    preview.drawnAt = now;
    preview.stale = false;
}

void OverviewDeskBarNode::renderBar(render::Renderer &renderer, const Bar &bar, double progress,
                                    double now) const
{
    const double scale = renderer.rasterScale();
    const double offset = snap(slideOffset(bar, progress), scale);
    const float opacity = float(std::clamp(progress, 0.0, 1.0));
    const auto label = [&](const QString &text, const QPointF &centre, int maxWidth) {
        QSizeF size;
        render::Texture *texture = m_chromeCache.label(renderer, m_textPainter, text,
                                                       kLabelPixelSize, kWhite, maxWidth, &size);
        if (!texture)
            return;
        QRectF rect(QPointF(), size);
        rect.moveCenter(centre);
        renderer.drawTextureF(texture, snapped(rect, scale), opacity);
    };

    const auto nameUnder = [&](const QString &text, const QRectF &rect) {
        label(text, QPointF(rect.center().x(), rect.bottom() + kLabelGap + kLabelPixelSize * 0.75),
              int(rect.width()));
    };

    for (int index = 0; index < int(bar.desks.size()); ++index) {
        const Desk &desk = bar.desks[index];
        const QRectF rect = snapped(desk.bounds.translated(0, offset), scale);
        updatePreview(renderer, bar, index, now);
        const auto preview = m_previews.find(previewKey(bar, index));
        renderer.pushRoundedClip(render::RoundedShape {rect, kPreviewRadius});
        if (preview != m_previews.end() && preview->second.texture)
            renderer.drawTextureF(preview->second.texture.get(), rect, opacity);
        else
            renderer.fillRectF(rect, faded(kBlack, opacity));
        renderer.popRoundedClip();
        const render::Color outline = desk.active ? kRingColor
                : desk.highlighted              ? kSelectionColor
                                                : kPreviewOutlineColor;
        const double width = desk.active || desk.highlighted ? kRingWidth : 1;
        const double gap = desk.active || desk.highlighted ? kRingOffset : 0;
        renderer.drawRoundedOutline(
                render::RoundedShape {rect.adjusted(-gap, -gap, gap, gap), kPreviewRadius + gap},
                faded(outline, opacity), width * scale);
        nameUnder(desk.name, rect);
        if (desk.closeShown) {
            const QRectF button = snapped(closeButtonRect(rect), scale);
            const render::Color cross = desk.closeHovered ? kWhite : faded(kWhite, 0.8f);
            if (render::Texture *glyph = m_chromeCache.glyph(
                        renderer, OverviewChromeCache::Glyph::Close, button.size(), cross))
                renderer.drawTextureF(glyph, button, opacity);
        }
    }

    if (bar.newDesk.isEmpty())
        return;
    const QRectF rect = snapped(bar.newDesk.translated(0, offset), scale);
    // Frosted, as the title plate is, over the dimmed desk under it.
    render::GlassParams blur;
    blur.opacity = opacity;
    renderer.pushRoundedClip({rect, kPreviewRadius});
    renderer.drawBackgroundBlur(rect.toAlignedRect(), qRound(kPreviewRadius), rect.toAlignedRect(),
                                blur);
    renderer.popRoundedClip();
    fillRounded(renderer, rect, kPreviewRadius,
                faded(bar.newDeskHighlighted ? kButtonHighlightColor : kButtonColor, opacity));
    QRectF plus(0, 0, kPlusSize, kPlusSize);
    plus.moveCenter(rect.center());
    if (render::Texture *glyph = m_chromeCache.glyph(renderer, OverviewChromeCache::Glyph::Plus,
                                                     plus.size(), kWhite))
        renderer.drawTextureF(glyph, snapped(plus, scale), opacity);
}

void OverviewDeskBarNode::render(render::Renderer &renderer) const
{
    if (!m_overview->isActive() || !isVisible())
        return;
    const double now = renderer.frameTime >= 0 ? renderer.frameTime : m_overview->clockTime();
    const QRect viewport = viewportOf(renderer, *this);
    const double progress = m_overview->progress(now);
    m_previewsLeft = kPreviewsPerFrame;
    for (const Bar &bar : m_bars) {
        if (bar.output.intersects(viewport))
            renderBar(renderer, bar, progress, now);
    }
    // A window dragged to a desk goes over the bar.
    m_overview->renderDragged(renderer);
}

void OverviewDeskBarNode::releaseRenderResources()
{
    m_previews.clear();
    m_chromeCache.clear();
    Node::releaseRenderResources();
}

} // namespace cutefish::scene
