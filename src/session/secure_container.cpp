// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/secure_container.h"

#include "scene/node.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QSettings>
#include <QUrl>
#include <QtConcurrentRun>
#include <algorithm>
#include <chrono>

namespace cutefish::session {

namespace {

constexpr render::Color kShieldColor {0.0f, 0.0f, 0.0f, 1.0f};

// A lock whose first frame comes this late has no screen showing its start;
// it goes straight to its end instead.
constexpr double kLateStartMs = 250;

// The backdrop and panel fade in over this, while the panel's raise runs the
// whole kCoverDurationMs. Leaving, the panel is gone before the backdrop.
constexpr double kCoverFadeMs = 300;
constexpr double kPanelLiftFadeMs = 200;
constexpr double kPanelRaiseFrom = 0.92;
constexpr double kPanelLiftTo = 1.06;

double steadyNow()
{
    return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

// gfx::Tween::EASE_IN_OUT and EASE_OUT, which ash's session state animator
// gives opacity and transform.
double easeInOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
}

double easeOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t);
}

QImage blurPass(const QImage &source, int radius, bool horizontal)
{
    QImage result(source.size(), source.format());
    const int length = horizontal ? source.width() : source.height();
    const int lines = horizontal ? source.height() : source.width();
    const int samples = radius * 2 + 1;
    for (int line = 0; line < lines; ++line) {
        const auto pixel = [&](int position) {
            position = qBound(0, position, length - 1);
            return horizontal ? source.pixel(position, line) : source.pixel(line, position);
        };
        int red = 0, green = 0, blue = 0, alpha = 0;
        const auto accumulate = [&](QRgb value, int sign) {
            red += qRed(value) * sign;
            green += qGreen(value) * sign;
            blue += qBlue(value) * sign;
            alpha += qAlpha(value) * sign;
        };
        for (int offset = -radius; offset <= radius; ++offset)
            accumulate(pixel(offset), 1);
        for (int position = 0; position < length; ++position) {
            const QRgb value =
                    qRgba(red / samples, green / samples, blue / samples, alpha / samples);
            if (horizontal)
                result.setPixel(position, line, value);
            else
                result.setPixel(line, position, value);
            accumulate(pixel(position - radius), -1);
            accumulate(pixel(position + radius + 1), 1);
        }
    }
    return result;
}

QImage loadWallpaper(const QSize &size)
{
    QImage image;
    QSettings settings(QStringLiteral("cutefish"), QStringLiteral("theme"));
    const QSize target(qMax(1, size.width() / 8), qMax(1, size.height() / 8));
    if (settings.value(QStringLiteral("BackgroundType"), 0).toInt() == 0) {
        QString path =
                settings.value(QStringLiteral("Wallpaper"),
                               QStringLiteral("/usr/share/backgrounds/cutefishos/default.jpg"))
                        .toString();
        if (QUrl(path).isLocalFile())
            path = QUrl(path).toLocalFile();
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (reader.size().isValid())
            reader.setScaledSize(reader.size().scaled(target, Qt::KeepAspectRatioByExpanding));
        image = reader.read();
        if (image.isNull())
            return {};
        image = image.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        image = image.copy((image.width() - target.width()) / 2,
                           (image.height() - target.height()) / 2, target.width(), target.height());
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        for (int pass = 0; pass < 3; ++pass) {
            image = blurPass(image, 3, true);
            image = blurPass(image, 3, false);
        }
    } else {
        image = QImage(target, QImage::Format_ARGB32_Premultiplied);
        image.fill(
                QColor(settings.value(QStringLiteral("BackgroundColor"), QStringLiteral("#2B8ADA"))
                               .toString()));
    }
    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    return image;
}

class WallpaperNode final : public scene::Node
{
public:
    WallpaperNode(const SecureContainer *container, const QSize &size)
        : Node(QStringLiteral("lock-wallpaper"))
        , m_container(container)
    {
        QObject::connect(&m_loader, &QFutureWatcher<QImage>::finished, &m_loader, [this] {
            m_image = m_loader.result();
            m_loaded = true;
            notifyChanged();
        });
        m_loader.setFuture(QtConcurrent::run(loadWallpaper, size));
        setGeometry(QRect(QPoint(), size));
    }

    Content content() const override { return Content::Translucent; }

    bool isLoaded() const { return m_loaded; }

    void releaseRenderResources() override { m_texture.reset(); }

protected:
    void paint(render::Renderer &renderer, const QRect &bounds) const override
    {
        if (m_image.isNull())
            return;
        if (!m_texture)
            m_texture =
                    renderer.createTexture(m_image.size(), m_image.constBits(),
                                           m_image.bytesPerLine(), render::PixelFormat::Rgba8888);
        if (m_texture)
            renderer.drawTexture(m_texture.get(), bounds, m_container->sample(renderer).backdrop);
    }

private:
    const SecureContainer *m_container;
    QFutureWatcher<QImage> m_loader;
    QImage m_image;
    bool m_loaded = false;
    mutable std::unique_ptr<render::Texture> m_texture;
};

// Black under the wallpaper, and in its place until it has loaded. It fades in
// only when there is no wallpaper to fade: under one it would darken the fade.
class ShieldNode final : public scene::ColorNode
{
public:
    explicit ShieldNode(const SecureContainer *container)
        : ColorNode(kShieldColor, QStringLiteral("shield"))
        , m_container(container)
    {
    }

    void render(render::Renderer &renderer) const override
    {
        Node::render(renderer);
        const double time = renderer.frameTime >= 0 ? renderer.frameTime : steadyNow();
        if (isVisible() && !m_container->isSettled(time))
            m_container->requestAnimationFrame();
    }

protected:
    void paint(render::Renderer &renderer, const QRect &bounds) const override
    {
        const float backdrop = m_container->sample(renderer).backdrop;
        const bool wallpaper = std::any_of(children().begin(), children().end(), [](auto *child) {
            const auto *node = dynamic_cast<const WallpaperNode *>(child);
            return node && node->isLoaded();
        });
        const float alpha = wallpaper ? (backdrop >= 1 ? 1 : 0) : backdrop;
        if (alpha > 0 && !bounds.isEmpty())
            renderer.fillRect(bounds, {0, 0, 0, alpha});
    }

private:
    const SecureContainer *m_container;
};

} // namespace

SecureContainer::SecureContainer(scene::Node *root)
    : m_root(root)
{
    m_shield = m_root->addChild(std::make_unique<scene::Node>(QStringLiteral("lock-shield")));
    m_auth = m_root->addChild(std::make_unique<scene::Node>(QStringLiteral("auth-ui")));
    m_root->setVisible(false);
    const auto changed = [this] {
        if (!m_blocked)
            refreshWallpaper();
    };
    QObject::connect(&m_wallpaperWatcher, &QFileSystemWatcher::directoryChanged,
                     &m_wallpaperWatcher, changed);
    QObject::connect(&m_wallpaperWatcher, &QFileSystemWatcher::fileChanged, &m_wallpaperWatcher,
                     changed);
    refreshWallpaper();
}

SecureContainer::~SecureContainer() = default;

void SecureContainer::setBlocked(bool blocked)
{
    if (m_blocked == blocked)
        return;
    m_blocked = blocked;
    m_root->setVisible(blocked);
    if (blocked)
        refreshWallpaper();
}

void SecureContainer::setCovered(bool covered, bool animate)
{
    m_covered = covered;
    m_animating = animate;
    m_requested = steadyNow();
    m_start.reset();
    requestAnimationFrame();
}

void SecureContainer::finishTransition()
{
    if (!m_animating)
        return;
    m_animating = false;
    requestAnimationFrame();
}

int SecureContainer::transitionDuration() const
{
    return m_covered ? kCoverDurationMs : kUncoverDurationMs;
}

CoverSample SecureContainer::sample(const render::Renderer &renderer) const
{
    return sample(renderer.frameTime, renderer.framePeriod);
}

CoverSample SecureContainer::sample(double frameTime, double framePeriod) const
{
    const double time = frameTime >= 0 ? frameTime : steadyNow();
    m_lastSample = time;
    const double duration = transitionDuration();
    if (m_animating && !m_start) {
        // Already moving in its first frame, as window animations do.
        m_start = time - kLateStartMs > m_requested ? time - duration : time - framePeriod;
    }
    const bool over = !m_animating || time >= *m_start + duration;
    if (over)
        return m_covered ? CoverSample {} : CoverSample {0, 0, 1};

    const double elapsed = time - *m_start;
    if (m_covered) {
        const double fade = easeInOut(elapsed / kCoverFadeMs);
        const double raise = easeOut(elapsed / kCoverDurationMs);
        return {float(fade), float(fade), kPanelRaiseFrom + (1 - kPanelRaiseFrom) * raise};
    }
    const double lift = easeOut(elapsed / kUncoverDurationMs);
    return {float(1 - easeInOut(elapsed / kUncoverDurationMs)),
            float(1 - easeInOut(elapsed / kPanelLiftFadeMs)), 1 + (kPanelLiftTo - 1) * lift};
}

bool SecureContainer::isSettled(double frameTime) const
{
    if (!m_animating)
        return true;
    return m_start && frameTime >= *m_start + transitionDuration();
}

void SecureContainer::requestAnimationFrame() const
{
    for (const auto *shield : m_shieldRects)
        shield->notifyChanged();
    for (const auto *panel : m_auth->children())
        panel->notifyChanged();
}

void SecureContainer::setOutputs(const QList<SecureOutput> &outputs)
{
    if (m_outputs == outputs)
        return;
    m_outputs = outputs;
    rebuildShield();
}

bool SecureContainer::isWallpaperReady() const
{
    if (m_outputs.isEmpty() || m_shieldRects.size() != size_t(m_outputs.size()))
        return false;
    return std::all_of(m_shieldRects.begin(), m_shieldRects.end(), [](const scene::ColorNode *shield) {
        return std::all_of(shield->children().begin(), shield->children().end(),
                           [](const scene::Node *child) {
                               const auto *wallpaper = dynamic_cast<const WallpaperNode *>(child);
                               return !wallpaper || wallpaper->isLoaded();
                           });
    });
}

void SecureContainer::refreshWallpaper()
{
    QSettings settings(QStringLiteral("cutefish"), QStringLiteral("theme"));
    const QString path =
            settings.value(QStringLiteral("Wallpaper"),
                           QStringLiteral("/usr/share/backgrounds/cutefishos/default.jpg"))
                    .toString();
    const QString file = QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path;
    if (QCoreApplication::instance()) {
        // The settings file and the wallpaper are replaced by rename, which
        // drops their watches; the directories see the rename.
        QStringList wanted {settings.fileName(), QFileInfo(settings.fileName()).absolutePath(),
                            file, QFileInfo(file).absolutePath()};
        wanted.removeIf([](const QString &path) { return !QFileInfo::exists(path); });
        const QStringList watched = m_wallpaperWatcher.files() + m_wallpaperWatcher.directories();
        QStringList stale = watched;
        stale.removeIf([&](const QString &path) { return wanted.contains(path); });
        wanted.removeIf([&](const QString &path) { return watched.contains(path); });
        if (!stale.isEmpty())
            m_wallpaperWatcher.removePaths(stale);
        if (!wanted.isEmpty())
            m_wallpaperWatcher.addPaths(wanted);
    }
    const QString key = path + QLatin1Char('|')
            + settings.value(QStringLiteral("BackgroundType"), 0).toString() + QLatin1Char('|')
            + settings.value(QStringLiteral("BackgroundColor"), QStringLiteral("#2B8ADA"))
                      .toString()
            + QLatin1Char('|')
            + QString::number(QFileInfo(file).lastModified().toMSecsSinceEpoch());
    if (key == m_wallpaperKey)
        return;
    m_wallpaperKey = key;
    rebuildShield();
}

void SecureContainer::rebuildShield()
{
    while (m_shieldRects.size() > size_t(m_outputs.size())) {
        m_shield->removeChild(m_shieldRects.back());
        m_shieldRects.pop_back();
    }
    while (m_shieldRects.size() < size_t(m_outputs.size())) {
        auto node = std::make_unique<ShieldNode>(this);
        m_shieldRects.push_back(
                static_cast<scene::ColorNode *>(m_shield->addChild(std::move(node))));
    }
    for (int i = 0; i < m_outputs.size(); ++i) {
        auto *shield = m_shieldRects[size_t(i)];
        shield->setGeometry(m_outputs.at(i).bounds);
        while (!shield->children().empty())
            shield->removeChild(shield->children().back());
        if (QCoreApplication::instance())
            shield->addChild(
                    std::make_unique<WallpaperNode>(this, m_outputs.at(i).bounds.size()));
    }
}

} // namespace cutefish::session
