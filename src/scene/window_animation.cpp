// Copyright 2013 The Chromium Authors
// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/window_animations.cc
//           chromium/src/ui/wm/core/window_animations.cc
//           chromium/src/ui/gfx/animation/tween.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#include "scene/window_animation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "scene/motion.h"

namespace cutefish::scene {

namespace {

QRectF shrink(const QRectF &bounds, qreal scale)
{
    const QSizeF size = bounds.size() * scale;
    return QRectF(bounds.center() - QPointF(size.width(), size.height()) / 2, size);
}

double smoothStep(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * t * (10 + t * (-15 + 6 * t));
}

double easeOut(double t, double power)
{
    return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), power);
}

// How much larger than the output a launcher is when fully transparent.
constexpr qreal kLauncherZoom = 1.05;

// gfx::Tween::ACCEL_20_DECEL_60: moving from the first frame, with no dwell
// at either end, so the motion fills the whole duration and lands softly.
double accel20Decel60(double t)
{
    return motion::cubicBezier(.2, 0, .4, 1, t);
}

// Windows open and close on critically damped springs: response in seconds.
// The fade leads the scale, so the first frames already show most of it.
constexpr double kOpenMotion = .36;
constexpr double kOpenFade = .12;
constexpr double kCloseMotion = .26;
constexpr double kCloseFade = .16;
constexpr int kOpenDurationMs = 360;
constexpr int kCloseDurationMs = 220;
// A preview growing into the window it stands for, on the ease-out of a window
// opening, without the spring's overshoot.
constexpr int kScaleDurationMs = 250;

// A spring's step response from rest `elapsed` ms in. Early on it runs in real
// time; its tail is warped to end at `duration` with no speed left, instead of
// spending frames on the last fraction of a pixel.
double springStep(double response, double elapsed, double duration)
{
    const double t = elapsed / duration;
    if (t <= 0)
        return 0;
    if (t >= 1)
        return 1;
    motion::Spring spring({response, 1});
    spring.reset(0, 0, 1);
    return std::clamp(spring.value(elapsed / (1 - t * t)), 0.0, 1.0);
}

QRect subtreeBounds(const Node *node)
{
    QRect bounds = node->absolutePaintBounds();
    for (const Node *child : node->children()) {
        if (child->isVisible())
            bounds = bounds.united(subtreeBounds(child));
    }
    return bounds;
}

// A surface on a hardware plane is left out of the texture for this frame only.
bool hasPromotedSurface(const Node &node)
{
    if (node.isPromotedToOverlay())
        return true;
    return std::any_of(node.children().begin(), node.children().end(), [](const Node *child) {
        return hasPromotedSurface(*child);
    });
}

// What SurfaceNode::paint records, for surfaces reaching the screen through a
// texture drawn from them earlier.
void recordPresentation(render::Renderer &renderer, const Node &node)
{
    if (!node.isVisible())
        return;
    if (const auto *surface = dynamic_cast<const SurfaceNode *>(&node);
        surface && surface->texture() && surface->alpha() > 0
        && !node.absoluteGeometry().isEmpty())
        renderer.recordPresentation(surface->presentationContent(), node.absoluteGeometry());
    for (const Node *child : node.children())
        recordPresentation(renderer, *child);
}

const Node &sceneRoot(const Node &node)
{
    const Node *root = &node;
    while (root->parent())
        root = root->parent();
    return *root;
}

// The viewport the scene's damage tracker is given for the output drawn now.
QRect outputViewport(const render::Renderer &renderer, const Node &node)
{
    const QRect viewport = renderer.outputContext().logicalViewport;
    return viewport.isEmpty() ? sceneRoot(node).geometry() : viewport;
}

} // namespace

WindowAnimation::WindowAnimation(std::function<double()> clock)
    : Node(QStringLiteral("window"))
    , m_now(clock ? std::move(clock) : [] {
        return std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
    })
{
}

int WindowAnimation::duration(Type type, const QRectF &from, const QRectF &to, Style style)
{
    if (style == Style::Launcher && type == Type::Open)
        return 360;
    if (style == Style::Launcher && type == Type::Close)
        return 240;
    // Menus open under the pointer and are read immediately: shorter than the
    // 150 ms ui/wm gives WINDOW_TYPE_MENU so the fade never reads as a delay.
    if (style == Style::Popup && type == Type::Open)
        return 100;
    if (style == Style::Popup && type == Type::Close)
        return 80;
    if (type == Type::Open)
        return qRound(kOpenDurationMs * motion::durationScale());
    if (type == Type::Close)
        return qRound(kCloseDurationMs * motion::durationScale());
    if (type == Type::Minimize)
        return 320;
    if (type == Type::Restore)
        return 320;
    // Ash's kCrossFadeDuration. A drag cross-fade is a flourish on top of a
    // gesture that is still running, so it is much shorter than a state change
    // the user only watches.
    if (type == Type::DragBounds)
        return 120;
    const double a = from.width() * from.height();
    const double b = to.width() * to.height();
    const double largest = std::max(a, b);
    return 200 + (largest > 0 ? qRound(200 * std::abs(a - b) / largest) : 0);
}

WindowAnimation::Sample WindowAnimation::sample(Type type, const QRectF &from, const QRectF &to,
                                                double progress, Style style)
{
    const double t = std::clamp(progress, 0.0, 1.0);
    if (style == Style::Launcher && (type == Type::Open || type == Type::Close)) {
        // Decelerating on both edges: the launcher responds on the first frame
        // and lands without a visible stop.
        const double eased = easeOut(t, type == Type::Open ? 4 : 3);
        const float opacity = type == Type::Open ? easeOut(t / .6, 3) : 1 - easeOut(t / .85, 2);
        return {QRectF(from.topLeft() + (to.topLeft() - from.topLeft()) * eased,
                       from.size() + (to.size() - from.size()) * eased),
                opacity, t};
    }
    if (style == Style::Popup && (type == Type::Open || type == Type::Close)) {
        // No motion: the popup is drawn where it is and only its opacity moves,
        // decelerating so the first frame already shows most of the change.
        const float opacity = type == Type::Open ? easeOut(t, 2) : 1 - easeOut(t, 2);
        return {to, opacity, t};
    }
    if (style == Style::Fade && (type == Type::Open || type == Type::Close)) {
        const float opacity = type == Type::Open ? smoothStep(t / .55) : 1 - smoothStep(t);
        return {to, opacity, t};
    }
    // Upstream passes gfx::Tween::LINEAR for the drag cross-fades. Bounds
    // changes land like ash's EASE_OUT, but ramp up over the first frames
    // instead of starting at full speed.
    double eased = type == Type::DragBounds ? t : accel20Decel60(t);
    float opacity = 1;
    // Minimize stays opaque: the window lands on the dock thumbnail that shows
    // the same image. Restore only fades in when it interrupts a fade.
    if (type == Type::Restore) {
        opacity = smoothStep(t / .3);
    } else if (type == Type::Open || type == Type::Close) {
        const double length = duration(type, from, to, style);
        const double elapsed = t * length;
        const bool open = type == Type::Open;
        eased = springStep(open ? kOpenMotion : kCloseMotion, elapsed, length);
        const double fade = springStep(open ? kOpenFade : kCloseFade, elapsed, length);
        opacity = open ? fade : 1 - fade;
    }
    return {QRectF(from.topLeft() + (to.topLeft() - from.topLeft()) * eased,
                   from.size() + (to.size() - from.size()) * eased),
            opacity, t};
}

WindowAnimation::Sample WindowAnimation::current() const
{
    const double t = (m_started >= 0)
            ? (now() - m_started) / duration(m_type, m_from, m_to, m_style)
            : 0;
    Sample result = sample(m_type, m_from, m_to, t, m_style);
    if (m_type == Type::Minimize && result.progress < 1) {
        // Replace each new endpoint's contribution with a smooth correction
        // over the remaining time, leaving the preceding trajectory intact.
        for (const auto &adjustment : m_minimizeAdjustments) {
            const double remaining = (result.progress - adjustment.progress)
                    / (1 - adjustment.progress);
            const double weight = smoothStep(remaining) - accel20Decel60(result.progress);
            result.bounds.translate(adjustment.position * weight);
            result.bounds.setSize(result.bounds.size() + adjustment.size * weight);
        }
    }
    if (m_type == Type::Close)
        result.opacity *= m_startOpacity;
    if (m_type == Type::Minimize)
        result.opacity = m_startOpacity + (1 - m_startOpacity) * smoothStep(result.progress / .6);
    if (m_type == Type::Restore || m_type == Type::Open)
        result.opacity = m_startOpacity + (1 - m_startOpacity) * result.opacity;
    if (isCrossFade(m_type))
        result.opacity = m_startOpacity + (1 - m_startOpacity) * smoothStep(result.progress);
    return result;
}

void WindowAnimation::finish()
{
    m_running = false;
    m_holding = false;
    m_firstFrame = false;
    m_boundsContentReady = true;
    m_oldTexture.reset();
    m_landingTexture.reset();
    m_snapshot = nullptr;
    m_landingSnapshot = nullptr;
    changed();
}

void WindowAnimation::changed() const
{
    notifyChanged();
    // A hidden node asks for no frame of its own, yet a minimize plays on.
    if (!isVisible() && parent())
        parent()->requestFrame();
}

void WindowAnimation::releaseRenderResources()
{
    finish();
    m_texture.reset();
    m_textureReusable = false;
    Node::releaseRenderResources();
}

void WindowAnimation::start(Type type, const QRect &from, const QRect &to, bool requestFrame)
{
    if (m_style == Style::None) {
        m_type = type;
        finish();
        return;
    }
    const bool interrupted = m_running;
    const Sample previous = interrupted ? m_presented : Sample {from, 1, 0};
    m_from = interrupted ? previous.bounds : QRectF(from);
    m_to = to;
    m_startOpacity =
            !interrupted && type == Type::Open ? 0 : previous.opacity;
    m_type = type;
    m_holding = false;
    m_minimizeAdjustments.clear();
    m_landingTexture.reset();
    m_snapshot = nullptr;
    m_landingSnapshot = nullptr;
    if (type == Type::Minimize) {
        m_dockWindow = from;
        m_dockSlot = to;
    } else if (type == Type::Restore) {
        m_dockWindow = to;
        m_dockSlot = from;
    }
    m_boundsContentReady = true;
    m_running = true;
    m_firstFrame = false;
    // Start on the next presented opportunity, so a delayed client/event loop
    // cannot consume an animation before its first frame is drawn.
    m_started = -1;
    const bool launcher = m_style == Style::Launcher;
    const bool inPlace = m_style == Style::Popup || m_style == Style::Fade;
    if (type == Type::Open && !interrupted)
        m_from = inPlace ? QRectF(to) : shrink(to, launcher ? kLauncherZoom : .94);
    if (type == Type::Close)
        m_to = inPlace ? m_from : launcher ? shrink(from, kLauncherZoom) : shrink(m_from, .95);
    m_presented = {m_from, m_startOpacity, 0};
    if (type == Type::DragBounds && !QRectF(to).isEmpty()) {
        m_dragOffset = QRectF(from).topLeft() - QRectF(to).topLeft();
        m_dragScale =
                QSizeF(double(from.width()) / to.width(), double(from.height()) / to.height());
    }
    // Whatever m_texture holds after this, it was not drawn from the subtree's
    // current state.
    m_textureReusable = false;
    if (isCrossFade(type) && m_texture) {
        m_oldTexture = std::move(m_texture);
        m_oldCoverage = m_coverage;
        m_oldBounds = m_textureBounds;
        m_oldVisual = m_visual;
    }
    if (!isCrossFade(type)) {
        if (!m_texture && m_oldTexture) {
            m_texture = std::move(m_oldTexture);
            m_coverage = m_oldCoverage;
            m_textureBounds = m_oldBounds;
            m_visual = m_oldVisual;
        }
        m_oldTexture.reset();
    }
    if (requestFrame)
        changed();
}

void WindowAnimation::requestSnapshot(std::function<void(const QImage &)> callback,
                                      int maximumPixels)
{
    m_snapshot = std::move(callback);
    m_landingSnapshot = m_snapshot;
    m_snapshotPixels = std::max(1, maximumPixels);
    changed();
}

void WindowAnimation::readSnapshot(render::Renderer &renderer, bool landing) const
{
    const auto callback = std::exchange(landing ? m_landingSnapshot : m_snapshot, nullptr);
    if (!callback || !m_texture || m_textureBounds.isEmpty())
        return;
    // A missing dock uses a one-pixel target; keep its useful preview for a later shell.
    if (landing && (m_to.width() <= 1 || m_to.height() <= 1))
        return;
    QSizeF pixels = (landing ? m_to.size() : QSizeF(m_textureBounds.size())) * renderer.rasterScale();
    if (pixels.width() > m_snapshotPixels || pixels.height() > m_snapshotPixels)
        pixels.scale(m_snapshotPixels, m_snapshotPixels, Qt::KeepAspectRatio);
    const QSize size(std::max(1, landing ? int(std::ceil(pixels.width())) : qRound(pixels.width())),
                     std::max(1, landing ? int(std::ceil(pixels.height())) : qRound(pixels.height())));
    const double sx = size.width() / double(m_textureBounds.width());
    const double sy = size.height() / double(m_textureBounds.height());
    // The shadow margins of the coverage land outside the image.
    const QRectF target((m_coverage.x() - m_textureBounds.x()) * sx,
                        (m_coverage.y() - m_textureBounds.y()) * sy, m_coverage.width() * sx,
                        m_coverage.height() * sy);
    // Trilinear filtering, which the flight needs, blends in the next smaller
    // level and softens a still image; the dock shows this one texel per pixel.
    const double minification = std::min(m_texture->size().width() / target.width(),
                                          m_texture->size().height() / target.height());
    const int level = int(std::floor(std::log2(std::max(1.0, minification))));
    renderer.limitMipmapLevel(m_texture.get(), level);
    const QImage image = renderer.renderToImage(size, [&] {
        renderer.pushRoundedClip(bodyShape(m_textureBounds, QRectF(QPointF(), size)));
        renderer.drawTextureF(m_texture.get(), target);
        renderer.popRoundedClip();
    });
    renderer.limitMipmapLevel(m_texture.get(), -1);
    if (!image.isNull()) {
        if (landing) {
            const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            m_landingTexture =
                    renderer.createTexture(rgba.size(), rgba.constBits(), rgba.bytesPerLine(),
                                           render::PixelFormat::Rgba8888);
        }
        callback(image);
    }
}

void WindowAnimation::setMinimizeTarget(const QRect &target)
{
    if (m_type != Type::Minimize || (!m_running && !m_holding) || target.isEmpty()
        || m_to == QRectF(target))
        return;
    if (m_running && m_started >= 0) {
        const double progress = current().progress;
        // Introduce each target change with zero velocity and acceleration,
        // preserving the flight already in progress and its landing deadline.
        if (progress < 1) {
            m_minimizeAdjustments.push_back({QRectF(target).topLeft() - m_to.topLeft(),
                                            QSizeF(target.size()) - m_to.size(), progress});
        }
    }
    m_to = target;
    m_dockSlot = target;
    changed();
}

void WindowAnimation::releaseHold()
{
    if (!m_holding)
        return;
    m_holding = false;
    // Nothing draws or reads the snapshots of a minimized window again: a
    // restore draws the live window, and the preview is the window's own image.
    m_texture.reset();
    m_oldTexture.reset();
    m_landingTexture.reset();
    m_textureReusable = false;
    changed();
}

float WindowAnimation::exteriorOpacity(const Sample &sample) const
{
    if (!isDockTransition())
        return 1;
    const qreal range = m_dockWindow.width() - m_dockSlot.width();
    if (range <= 0)
        return 1;
    return std::clamp((sample.bounds.width() - m_dockSlot.width()) / range, qreal(0), qreal(1));
}

render::RoundedShape WindowAnimation::bodyShape(const QRect &original, const QRectF &bounds) const
{
    if (m_visual.shape.bounds.isEmpty())
        return render::RoundedShape {bounds};
    return transformedShape(m_visual.shape, original, Sample {bounds, 1, 1});
}

bool WindowAnimation::morphsExterior() const
{
    const auto plain = [](const WindowVisualState &visual) {
        return visual.mode != WindowVisualMode::PreserveClient && visual.outlinePath.isEmpty()
                && visual.outlineShape.bounds.isEmpty() && !visual.shape.bounds.isEmpty();
    };
    return m_running && m_type == Type::Bounds && m_oldTexture && plain(m_visual)
            && plain(m_oldVisual);
}

qreal WindowAnimation::morphedRadius(const Sample &sample) const
{
    const qreal from = m_oldVisual.shape.radius;
    return from + (m_visual.shape.radius - from) * accel20Decel60(sample.progress);
}

void WindowAnimation::drawExteriorMorph(render::Renderer &renderer, const Sample &value,
                                        float blend) const
{
    const float progress = accel20Decel60(value.progress);
    // Only a window in its normal state has a shadow and an outline.
    const bool oldExterior = m_oldVisual.drawOutline;
    const bool newExterior = m_visual.drawOutline;
    render::RoundedShape body = transformedShape(m_visual.shape, m_textureBounds, value);
    body.radius = morphedRadius(value);
    body.radiusScale = QSizeF(1, 1);

    // The old shadow fades out under the window growing out of it; its corners
    // are never tighter than the body's, so none of its content shows.
    if (oldExterior && !newExterior)
        draw(renderer, m_oldTexture.get(), m_oldCoverage, m_oldBounds, value, 1 - progress);
    const float exterior = newExterior ? (oldExterior ? 1 : progress) : 0;
    if (exterior > 0)
        draw(renderer, m_texture.get(), m_coverage, m_textureBounds, value, exterior);
    renderer.pushRoundedClip(body);
    if (exterior < 1)
        draw(renderer, m_texture.get(), m_coverage, m_textureBounds, value);
    draw(renderer, m_oldTexture.get(), m_oldCoverage, m_oldBounds, value, 1 - blend);
    renderer.popRoundedClip();

    const float outline = (oldExterior ? 1 - progress : 0) + (newExterior ? progress : 0);
    if (outline > 0 && !renderer.suppressWindowOutlines)
        paintWindowOutline(renderer, body, newExterior ? m_visual : m_oldVisual,
                           outline * value.opacity);
}

void WindowAnimation::setDragTarget(const QRect &bounds)
{
    if (m_type != Type::DragBounds || !m_running || bounds.isEmpty() || m_to == QRectF(bounds))
        return;
    m_to = bounds;
    m_from = QRectF(
            m_to.topLeft() + m_dragOffset,
            QSizeF(m_to.width() * m_dragScale.width(), m_to.height() * m_dragScale.height()));
    changed();
}

void WindowAnimation::setBoundsContentReady(bool ready)
{
    if (!isCrossFade(m_type) || m_boundsContentReady == ready)
        return;
    m_boundsContentReady = ready;
    changed();
}

void WindowAnimation::setStyle(Style style)
{
    m_style = style;
    if (style != Style::Launcher || !m_firstFrame || !parent())
        return;
    for (Node *sibling : parent()->children()) {
        auto *closing = dynamic_cast<WindowAnimation *>(sibling);
        if (!closing || closing == this || closing->m_style != Style::Launcher
            || !closing->m_closing || !closing->m_running || closing->m_textureBounds.isEmpty())
            continue;
        const QRectF snapshot = closing->m_textureBounds;
        const QRectF presented = closing->m_presented.bounds;
        m_handoff = Sample {QRectF((presented.x() - snapshot.x()) / snapshot.width(),
                                   (presented.y() - snapshot.y()) / snapshot.height(),
                                   presented.width() / snapshot.width(),
                                   presented.height() / snapshot.height()),
                            closing->m_presented.opacity, 0};
        // Retired before either paints again, so no frame draws both.
        closing->m_running = false;
        closing->changed();
        return;
    }
}

void WindowAnimation::setOpacity(float opacity)
{
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (m_opacity == opacity)
        return;
    m_opacity = opacity;
    changed();
}

void WindowAnimation::setPresentationScale(double scale, const QPointF &anchor, bool animate)
{
    m_scaleFrom = animate ? presentationScale() : scale;
    m_scaleTo = scale;
    m_scaleStarted = -1;
    m_scaleAnchor = anchor;
    // Whether the snapshot has its blurs may change.
    m_textureReusable = false;
    changed();
}

double WindowAnimation::presentationScale() const
{
    if (m_scaleFrom == m_scaleTo || m_scaleStarted < 0)
        return m_scaleFrom;
    const double t = (now() - m_scaleStarted) / (kScaleDurationMs * motion::durationScale());
    return t >= 1 ? m_scaleTo : m_scaleFrom + (m_scaleTo - m_scaleFrom) * easeOut(t, 3);
}

void WindowAnimation::close()
{
    if (!isVisible() || (m_type == Type::Minimize && !m_running) || (!m_texture && !m_oldTexture)) {
        m_running = false;
        m_holding = false;
        m_closing = true;
        changed();
        return;
    }
    // The snapshot is scaled relative to where it was captured; the live
    // geometry may have changed since the last paint.
    const QRect snapshot = m_texture ? m_textureBounds : m_oldBounds;
    start(Type::Close, snapshot, snapshot);
    m_closing = true;
}

void WindowAnimation::draw(render::Renderer &renderer, render::Texture *texture,
                           const QRect &coverage, const QRect &original, const Sample &value,
                           float alpha) const
{
    if (!texture || original.isEmpty())
        return;
    const double sx = value.bounds.width() / original.width();
    const double sy = value.bounds.height() / original.height();
    const QPoint offset = coverage.topLeft() - original.topLeft();
    const QRectF target(value.bounds.topLeft() + QPointF(offset.x() * sx, offset.y() * sy),
                        QSizeF(coverage.width() * sx, coverage.height() * sy));
    renderer.drawTextureF(texture, target, value.opacity * alpha);
}

render::RoundedShape WindowAnimation::transformedShape(render::RoundedShape shape,
                                                       const QRect &original, const Sample &sample)
{
    if (original.isEmpty())
        return shape;
    const qreal sx = sample.bounds.width() / original.width();
    const qreal sy = sample.bounds.height() / original.height();
    shape.bounds =
            QRectF(sample.bounds.topLeft() + QPointF(shape.bounds.x() * sx, shape.bounds.y() * sy),
                   QSizeF(shape.bounds.width() * sx, shape.bounds.height() * sy));
    shape.radiusScale = QSizeF(sx, sy);
    return shape;
}

void WindowAnimation::outline(render::Renderer &renderer, const WindowVisualState &visual,
                              const QRect &original, const Sample &sample) const
{
    if (!visual.drawOutline || renderer.suppressWindowOutlines)
        return;
    if (!visual.outlinePath.isEmpty() && !original.isEmpty()) {
        QTransform transform;
        transform.translate(sample.bounds.x(), sample.bounds.y());
        transform.scale(sample.bounds.width() / original.width(),
                        sample.bounds.height() / original.height());
        paintShapeOutline(renderer, visual, transform, sample.opacity);
        return;
    }
    paintWindowOutline(renderer, transformedShape(visual.outlined(), original, sample), visual,
                       sample.opacity);
}

QRect WindowAnimation::absolutePaintBounds() const
{
    QRect bounds = absoluteGeometry();
    if (!paintsSubtree())
        return bounds;
    for (const auto *child : children()) {
        if (child->isVisible())
            bounds = bounds.united(subtreeBounds(child));
    }
    // A translucent window at rest blends its shadow too, and an opacity
    // change repaints it; the travel of a past animation is not drawn.
    if (!animating())
        return bounds.united(m_coverage);
    // Retired live nodes cannot describe the frozen shadow or its travel path.
    return bounds.united(m_coverage)
            .united(m_oldCoverage)
            .united(m_presented.bounds.toAlignedRect())
            .united(m_from.toAlignedRect())
            .united(m_to.toAlignedRect());
}

QRect WindowAnimation::textureCoverage() const
{
    // Include outlying subsurfaces and the SSD shadow's paint overflow.
    QRect coverage = absoluteGeometry();
    for (const auto *child : children()) {
        if (child->isVisible())
            coverage = coverage.united(subtreeBounds(child));
    }
    return coverage.adjusted(-64, -64, 64, 64);
}

bool WindowAnimation::textureReusable(const QRect &coverage, const QRect &viewport,
                                      double scale) const
{
    return m_textureReusable && m_texture && !animating() && coverage == m_coverage
            && scale == m_textureScale && viewport == m_textureViewport
            && sceneRoot(*this).revision() == m_textureRootRevision && !hasPromotedSurface(*this);
}

bool WindowAnimation::redrawsSubtree(const QRect &viewport, double scale) const
{
    // Hidden, or drawing a snapshot, render() makes no texture pass at all.
    if (!isVisible() || m_closing || m_type == Type::Minimize)
        return false;
    const QRect coverage = textureCoverage();
    if (!animating() && !viewport.isEmpty() && !coverage.intersects(viewport))
        return false;
    return !textureReusable(coverage, viewport, scale);
}

bool WindowAnimation::textureIsCurrent(render::Renderer &renderer, const QRect &coverage,
                                       QRect *repaint) const
{
    // The blurs in the texture read the output behind the window, which may
    // have changed wherever this frame draws it again, and nowhere else.
    const QRegion damage = m_textureDamage.update(*this, coverage, renderer.rasterScale(), 0,
                                                  [&](const QRect &sampled) {
                                                      return renderer.repaints(sampled);
                                                  });
    const bool valid =
            textureReusable(coverage, outputViewport(renderer, *this), renderer.rasterScale());
    *repaint = valid ? render::coverSamples(damage.boundingRect(), m_textureDamage.samples())
                     : QRect();
    return valid && damage.isEmpty();
}

void WindowAnimation::render(render::Renderer &renderer) const
{
    if ((!isVisible() && m_type != Type::Minimize && !m_closing) || isRetired())
        return;
    // Sampled per frame, not per draw: every output frame advances the
    // animation by what the display showed since the last one, however late
    // the wakeup or the uploads before this draw were.
    m_frameTime = renderer.frameTime;
    if (m_firstFrame) {
        auto *self = const_cast<WindowAnimation *>(this);
        const QRect geometry = absoluteGeometry();
        self->start(Type::Open, geometry, geometry, false);
        if (m_handoff) {
            const QRectF &relative = m_handoff->bounds;
            self->m_from = QRectF(geometry.x() + relative.x() * geometry.width(),
                                  geometry.y() + relative.y() * geometry.height(),
                                  relative.width() * geometry.width(),
                                  relative.height() * geometry.height());
            self->m_startOpacity = m_handoff->opacity;
            self->m_presented = {m_from, m_startOpacity, 0};
            self->m_handoff.reset();
        }
    }
    // A configure acknowledgement alone does not replace the old client buffer.
    // Keep its snapshot until the corresponding surface commit is applied.
    if (m_running && isCrossFade(m_type) && !m_boundsContentReady && m_oldTexture) {
        Sample held = m_presented;
        held.opacity *= m_opacity;
        draw(renderer, m_oldTexture.get(), m_oldCoverage, m_oldBounds, held);
        outline(renderer, m_oldVisual, m_oldBounds, held);
        return;
    }
    if (m_running && m_started < 0) {
        m_firstStepAt = now();
        // A window opening or closing already moves in its first frame: one
        // showing where it starts would only add a frame of latency.
        const bool leadIn =
                m_style == Style::Window && (m_type == Type::Open || m_type == Type::Close);
        m_started = leadIn ? m_firstStepAt - renderer.framePeriod : m_firstStepAt;
        m_firstStep = true;
    } else if (m_running && std::exchange(m_firstStep, false) && renderer.framePeriod > 0
               && now() - m_firstStepAt > renderer.framePeriod * 3 / 2) {
        // A slow first frame (snapshot pass, readback, buffer import) delays
        // the animation by the slots it missed instead of opening with a jump.
        m_started += now() - m_firstStepAt - renderer.framePeriod;
    }

    const bool frozen = m_closing || m_type == Type::Minimize;
    if (frozen && !m_texture) {
        m_snapshot = nullptr;
        m_landingSnapshot = nullptr;
        m_holding = false;
        if (std::exchange(m_running, false) && m_finished)
            m_finished();
        return;
    }
    if (!frozen) {
        for (const Node *child : children()) {
            if (const auto *visual = dynamic_cast<const WindowVisualNode *>(child))
                m_visual = visual->state();
        }
        const QRect coverage = textureCoverage();
        // An idle window on another output is not drawn here, so its snapshot
        // keeps the scale of an output that shows it.
        const QRect viewport = renderer.outputContext().logicalViewport;
        if (!animating() && !viewport.isEmpty() && !coverage.intersects(viewport))
            return;
        // Corners in texture space that the draw below scales to morphedRadius().
        std::optional<render::Renderer::WindowCorners> corners;
        if (morphsExterior()) {
            const Sample sample = current();
            const QRect geometry = absoluteGeometry();
            if (!sample.bounds.isEmpty()) {
                corners = render::Renderer::WindowCorners {
                        morphedRadius(sample),
                        QSizeF(geometry.width() / sample.bounds.width(),
                               geometry.height() / sample.bounds.height())};
            }
        }
        // The pass suspends presentation recording, which is right for a
        // snapshot kept for later. This one is drawn in the same frame, so the
        // live content in it is presented like any layer cc draws through a
        // render surface.
        render::PresentationSubmission *submission = renderer.presentationSubmission;
        QRect repaint;
        if (textureIsCurrent(renderer, coverage, &repaint)) {
            recordPresentation(renderer, *this);
        } else {
            // Mid-animation passes and plane promotions draw what only this
            // frame shows. A capture of the same output draws what the screen
            // does, so the two share the snapshot; one at another scale or
            // viewport fails textureReusable() anyway.
            const bool reusable = !animating() && !hasPromotedSurface(*this);
            if (!renderer.renderToTexture(m_texture, coverage, repaint, [&] {
                    const bool suppressed = renderer.suppressWindowOutlines;
                    const bool blurSuppressed = renderer.suppressBackgroundBlur;
                    renderer.suppressWindowOutlines = true;
                    renderer.suppressBackgroundBlur = blurSuppressed || presentedScaled();
                    renderer.presentationSubmission = submission;
                    renderer.windowCorners = corners;
                    Node::render(renderer);
                    renderer.windowCorners.reset();
                    renderer.suppressBackgroundBlur = blurSuppressed;
                    renderer.suppressWindowOutlines = suppressed;
                })) {
                m_textureReusable = false;
                Node::render(renderer);
                m_running = false;
                return;
            }
            // A blur drawn from what the last frame left behind the window is
            // not on screen now, but would be wrong later. The scene repaints
            // all a blur reads when the whole texture is drawn again next frame.
            const QRect drawn = repaint.isEmpty() ? coverage : repaint;
            const QRect output = outputViewport(renderer, *this);
            const auto &samples = m_textureDamage.samples();
            const bool backdropCurrent =
                    std::all_of(samples.begin(), samples.end(), [&](const QRect &sampled) {
                        return !sampled.intersects(drawn) || !sampled.intersects(output)
                                || renderer.repaints(sampled);
                    });
            m_textureReusable = reusable && backdropCurrent;
            m_textureScale = renderer.rasterScale();
            m_textureViewport = output;
            m_textureRootRevision = sceneRoot(*this).revision();
        }
        m_coverage = coverage;
        m_textureBounds = absoluteGeometry();
        // A popup repositioned by its client mid-fade is drawn where it now is.
        if (m_style == Style::Popup && m_running) {
            auto *self = const_cast<WindowAnimation *>(this);
            self->m_from = self->m_to = QRectF(m_textureBounds);
        }
    }

    // Shrunk to a dock slot, a bilinear snapshot shimmers; the dock images
    // are read through the same filtering, so the landing still matches.
    if (m_running && m_type == Type::Minimize)
        renderer.enableTrilinearFiltering(m_texture.get());
    if (m_snapshot && m_type == Type::Minimize)
        readSnapshot(renderer);

    Sample value = m_running || m_holding ? current() : Sample {absoluteGeometry(), 1, 1};
    // What was presented stays the animation's own; each draw adds the client's.
    m_presented = value;
    value.opacity *= m_opacity;
    if (m_scaleFrom != m_scaleTo && m_scaleStarted < 0)
        m_scaleStarted = now();
    if (const double scale = presentationScale(); scale != 1) {
        // Kept inside, so a shrunk window never draws outside its own bounds.
        const QRect geometry = absoluteGeometry();
        const QPointF anchor(geometry.x() + std::clamp<qreal>(m_scaleAnchor.x(), 0, geometry.width()),
                             geometry.y() + std::clamp<qreal>(m_scaleAnchor.y(), 0, geometry.height()));
        value.bounds = QRectF(anchor + (value.bounds.topLeft() - anchor) * scale,
                              value.bounds.size() * scale);
    }
    if (m_running && m_type == Type::Minimize && value.progress >= 1 && m_landingSnapshot)
        readSnapshot(renderer, true);

    if ((m_running || m_holding) && m_type == Type::Minimize && value.progress >= 1
        && m_landingTexture) {
        // The clip and resampling are already baked into the image sent to the dock.
        renderer.drawTextureF(m_landingTexture.get(), value.bounds, value.opacity);
    } else if (!frozen || m_running || m_holding) {
        const float blend =
                m_running && isCrossFade(m_type) ? 1 - std::pow(1 - value.progress, 2) : 1;
        // The current snapshot supplies the exterior once. Old content fades
        // only inside its silhouette, so a square old buffer cannot leak out.
        const float exterior = exteriorOpacity(value);
        if (morphsExterior()) {
            drawExteriorMorph(renderer, value, blend);
        } else {
            if (exterior < 1) {
                if (exterior > 0)
                    draw(renderer, m_texture.get(), m_coverage, m_textureBounds, value, exterior);
                renderer.pushRoundedClip(bodyShape(m_textureBounds, value.bounds));
                draw(renderer, m_texture.get(), m_coverage, m_textureBounds, value);
                renderer.popRoundedClip();
            } else {
                draw(renderer, m_texture.get(), m_coverage, m_textureBounds, value);
            }
            if (m_oldTexture) {
                if (m_visual.clipContent)
                    renderer.pushRoundedClip(
                            transformedShape(m_visual.shape, m_textureBounds, value));
                draw(renderer, m_oldTexture.get(), m_oldCoverage, m_oldBounds, value, 1 - blend);
                if (m_visual.clipContent)
                    renderer.popRoundedClip();
            }
            Sample outlined = value;
            outlined.opacity *= exterior;
            outline(renderer, m_visual, m_textureBounds, outlined);
        }
    }
    if (m_scaleFrom != m_scaleTo) {
        if (presentationScale() == m_scaleTo)
            m_scaleFrom = m_scaleTo;
        changed();
    }
    if (m_running) {
        if (value.progress >= 1) {
            m_running = false;
            m_holding = m_holdAtEnd && m_type == Type::Minimize;
            m_oldTexture.reset();
            if (m_finished)
                m_finished();
        }
        // Also repaint the final removal/hide. OutputScheduler coalesces these
        // requests with KMS presentation; no independent animation timer runs.
        changed();
    }
}

} // namespace cutefish::scene
