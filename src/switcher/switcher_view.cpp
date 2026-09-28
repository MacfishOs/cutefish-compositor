// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "switcher/switcher_view.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <QCoreApplication>
#include <QIcon>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

#include "core/logging.h"
#include "decoration/decoration_theme.h"
#include "decoration/shadow.h"
#include "decoration/shadow_node.h"
#include "display/display_manager.h"
#include "render/glass.h"
#include "render/renderer.h"
#include "scene/node.h"
#include "scene/window_visual_node.h"
#include "wm/window.h"
#include "wm/window_manager.h"

static void initializeSwitcherResources()
{
    Q_INIT_RESOURCE(switcher_qml);
}

namespace cutefish::switcher {

namespace {

// The list's icons, from the view's cache.
class IconProvider final : public QQuickImageProvider
{
public:
    explicit IconProvider(IconCache *icons)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_icons(icons)
    {
    }

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        const QString name = QUrl::fromPercentEncoding(id.toUtf8());
        const QImage image =
                m_icons->image(name, requestedSize.isValid() ? requestedSize.width() : 128);
        if (size)
            *size = image.size();
        return image;
    }

private:
    IconCache *m_icons;
};

// The offscreen platform the compositor runs on searches no icon theme at all.
void initializeIconThemes()
{
    QStringList paths;
    for (const QString &directory :
         QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        paths.append(directory + QLatin1String("/icons"));
    }
    paths.append(QDir::homePath() + QLatin1String("/.icons"));
    QIcon::setThemeSearchPaths(paths);
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    QIcon::setFallbackSearchPaths({QStringLiteral("/usr/share/pixmaps")});
}

// Frosted enough that a busy wallpaper under the icons and the name calms to
// soft patches, and tinted enough to stand off a flat colour of any shade;
// the rim still bends what is behind it.
render::GlassParams panelGlass(bool darkMode)
{
    render::GlassParams glass;
    glass.enabled = true;
    // The glass blur kernel is 30 device pixels wide; a larger sigma truncates.
    glass.blurSigma = 8.0f;
    glass.displacement = 18.0f;
    glass.chroma = 0.02f;
    glass.tintRed = darkMode ? 0.14f : 1.0f;
    glass.tintGreen = darkMode ? 0.14f : 1.0f;
    glass.tintBlue = darkMode ? 0.16f : 1.0f;
    glass.tintAlpha = darkMode ? 0.62f : 0.58f;
    return glass;
}

// The selection: a smaller, frosted piece of glass that darkens what it
// covers in the light and lifts it in the dark.
render::GlassParams selectionGlass(bool darkMode)
{
    render::GlassParams glass;
    glass.enabled = true;
    glass.blurSigma = 6.0f;
    glass.displacement = 12.0f;
    glass.chroma = 0.02f;
    glass.tintRed = glass.tintGreen = glass.tintBlue = darkMode ? 1.0f : 0.0f;
    glass.tintAlpha = darkMode ? 0.14f : 0.12f;
    return glass;
}

// The logical step whose multiples land on whole device pixels at `scale`.
int pixelStep(double scale)
{
    const int numerator = qRound(scale * 120);
    return numerator > 0 ? 120 / std::gcd(numerator, 120) : 1;
}

int snapDown(double value, int step)
{
    return int(std::floor(value / step)) * step;
}

int snapUp(double value, int step)
{
    return int(std::ceil(value / step)) * step;
}

class PanelNode final : public scene::Node
{
public:
    explicit PanelNode(SwitcherView *view)
        : Node(QStringLiteral("switcher-panel"))
        , m_view(view)
    {
    }

    Content content() const override { return Content::Translucent; }

    // The outline is at most one physical pixel wide, so one logical pixel covers it.
    QRect absolutePaintBounds() const override
    {
        return absoluteGeometry().adjusted(-1, -1, 1, 1);
    }

    void releaseRenderResources() override { m_texture.reset(); }

protected:
    void paint(render::Renderer &renderer, const QRect &bounds) const override
    {
        const QImage source = m_view->renderImage();
        if (source.isNull())
            return;
        if (!m_texture || m_uploaded != m_view->imageGeneration()) {
            // The list grows and shrinks with the applications it shows.
            if (!m_texture || m_texture->size() != source.size()
                || !renderer.updateTexture(m_texture.get(), source.constBits(),
                                           source.bytesPerLine(), render::PixelFormat::Rgba8888))
                m_texture = renderer.createTexture(source.size(), source.constBits(),
                                                   source.bytesPerLine(),
                                                   render::PixelFormat::Rgba8888);
            m_uploaded = m_view->imageGeneration();
        }
        const decoration::Theme *theme = m_view->theme();
        const bool darkMode = theme && theme->darkMode();
        renderer.drawBackgroundBlur(bounds, SwitcherView::kRadius, bounds, panelGlass(darkMode));
        if (const QRect selection = m_view->selectionBounds(); !selection.isEmpty()) {
            renderer.drawBackgroundBlur(selection, m_view->selectionRadius(), bounds,
                                        selectionGlass(darkMode));
        }
        if (m_texture)
            renderer.drawTextureMapped(m_texture.get(), bounds, 1.0f, QTransform());
        if (!theme)
            return;
        // The same edge the shell's glass surfaces get.
        const decoration::WindowVisualStyle style = theme->visualStyle();
        scene::WindowVisualState state;
        state.shape = {QRectF(bounds), qreal(SwitcherView::kRadius)};
        state.drawOutline = true;
        state.outlineWidthPx = style.outlineWidthPx;
        state.outlineColor = style.activeOutline;
        state.highlightColor = style.highlight;
        scene::paintWindowOutline(renderer, state.shape, state);
    }

private:
    SwitcherView *m_view;
    mutable std::unique_ptr<render::Texture> m_texture;
    mutable quint64 m_uploaded = 0;
};

} // namespace

QImage IconCache::image(const QString &name, int pixels)
{
    const QString key = name + QLatin1Char('@') + QString::number(pixels);
    if (const QImage *cached = m_images.object(key))
        return *cached;
    QIcon icon = name.startsWith(QLatin1Char('/')) ? QIcon(name) : QIcon::fromTheme(name);
    if (icon.isNull())
        icon = QIcon::fromTheme(kFallbackIcon);
    const QImage image = icon.pixmap(QSize(pixels, pixels)).toImage();
    m_images.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes()));
    return image;
}

SwitcherView::SwitcherView(scene::Node *container, wm::WindowManager *manager,
                           const decoration::Theme *theme, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
    , m_theme(theme)
{
    initializeSwitcherResources();
    if (m_theme) {
        m_shadow = static_cast<decoration::ShadowNode *>(
                container->addChild(std::make_unique<decoration::ShadowNode>(m_theme)));
        m_shadow->setVisible(false);
    }
    m_panel = container->addChild(std::make_unique<PanelNode>(this));
    m_panel->setVisible(false);

    m_warmTimer.setSingleShot(true);
    m_warmTimer.callOnTimeout(this, &SwitcherView::warmNext);
}

SwitcherView::~SwitcherView() = default;

bool SwitcherView::ensureScene()
{
    if (m_root)
        return true;
    initializeIconThemes();
    m_engine = std::make_unique<QQmlEngine>();
    m_engine->addImageProvider(QStringLiteral("appicon"), new IconProvider(&m_icons));
    m_control = std::make_unique<QQuickRenderControl>();
    m_window = std::make_unique<QQuickWindow>(m_control.get());
    m_window->setColor(Qt::transparent);
    connect(m_control.get(), &QQuickRenderControl::renderRequested, this, &SwitcherView::markDirty);
    connect(m_control.get(), &QQuickRenderControl::sceneChanged, this, &SwitcherView::markDirty);
    QQmlComponent component(m_engine.get(), QUrl(QStringLiteral("qrc:/switcher/Switcher.qml")));
    QObject *object = component.create();
    m_root = qobject_cast<QQuickItem *>(object);
    if (!m_root) {
        qCWarning(cfCompositor) << "The application switcher did not load:"
                                << component.errorString();
        delete object;
        return false;
    }
    m_root->setParent(m_window->contentItem());
    m_root->setParentItem(m_window->contentItem());
    m_root->setProperty("padding", kPadding);
    m_root->setProperty("iconSourceSize", kCellSize * kIconRatio);
    return true;
}

void SwitcherView::markDirty()
{
    // Rendering itself reports changes; those are already in the image.
    if (m_rendering)
        return;
    m_dirty = true;
    if (m_shown)
        m_panel->notifyChanged();
}

void SwitcherView::preload()
{
    if (!ensureScene())
        return;
    // Drawing a name and an icon once loads the fonts and the icon theme, which
    // cost far more than the engine on the first switch.
    const auto [area, scale] = outputFor(QString());
    layout({QVariantMap {{QStringLiteral("name"), QStringLiteral("Aa")},
                         {QStringLiteral("icon"), kFallbackIcon}}},
           0, area, scale);
    render();

    for (wm::Window *window : m_manager->windows())
        m_warmQueue.push_back(window->id());
    connect(m_manager, &wm::WindowManager::windowAdded, this, [this](wm::Window *window) {
        m_warmQueue.push_back(window->id());
        if (!m_warmTimer.isActive())
            m_warmTimer.start(kWarmDelayMs);
    });
    if (!m_warmQueue.empty())
        m_warmTimer.start(0);
}

int SwitcherView::iconPixels(double scale)
{
    return qRound(kCellSize * kIconRatio * scale);
}

void SwitcherView::followIconTheme()
{
    const QSettings settings(QSettings::UserScope, QStringLiteral("cutefish"),
                             QStringLiteral("theme"));
    const QString theme =
            settings.value(QStringLiteral("IconTheme"), QStringLiteral("Cutefish")).toString();
    if (theme == m_iconTheme)
        return;
    m_iconTheme = theme;
    QIcon::setThemeName(theme);
    m_icons.clear();
}

void SwitcherView::warmNext()
{
    if (m_warmQueue.empty())
        return;
    const quint64 id = m_warmQueue.front();
    m_warmQueue.pop_front();
    wm::Window *window = m_manager->windowById(id);
    if (window && !window->skipTaskbar()) {
        m_applications.refresh();
        const ApplicationInfo info =
                m_applications.lookup(window->applicationId(), window->title());
        m_icons.image(info.icon, iconPixels(outputFor(QString()).second));
    }
    // One window per turn of the event loop, so no frame waits for many.
    if (!m_warmQueue.empty())
        m_warmTimer.start(0);
}

std::pair<QRect, double> SwitcherView::outputFor(const QString &output) const
{
    QRect area = m_manager->outputForNewWindow();
    if (const display::DisplayManager *displays = m_manager->displayManager();
        displays && displays->applied()) {
        for (const auto &logical : displays->applied()->logicalDisplays) {
            if (logical.waylandName == output || (output.isEmpty() && logical.bounds == area))
                return {logical.bounds, logical.scale.value()};
        }
    }
    return {area, 1};
}

void SwitcherView::showWindowCycle(const std::vector<wm::Window *> &entries, int selected,
                                   const QString &output)
{
    if (!ensureScene())
        return;
    m_applications.refresh();
    QVariantList list;
    for (wm::Window *window : entries) {
        const ApplicationInfo info = m_applications.lookup(window->applicationId(), window->title());
        list.append(QVariantMap {{QStringLiteral("name"), info.name},
                                 {QStringLiteral("icon"), info.icon}});
    }
    const auto [area, scale] = outputFor(output);
    layout(list, selected, area, scale);

    m_shown = true;
    m_panel->setGeometry(m_bounds);
    m_panel->setVisible(true);
    if (m_shadow) {
        // Lifted like a window: on a flat colour as light as the glass, the
        // shadow is all that draws the panel's edge. Shadows refuse a larger
        // radius than they cache; under a blur this wide the corner is the same.
        m_shadow->update(m_bounds, wm::ShadowElevation::Active, decoration::Corners(),
                         std::min(kRadius, decoration::ShadowGeometry::kMaxRadius));
        m_shadow->setVisible(true);
    }
    m_panel->notifyChanged();
}

void SwitcherView::layout(const QVariantList &entries, int selected, const QRect &area,
                          double scale)
{
    // Following the session's icon theme, which may have changed since.
    followIconTheme();

    m_count = static_cast<int>(entries.size());
    const int step = pixelStep(scale);
    const qreal available = area.width() - 2 * kScreenMargin - 2 * kPadding;
    m_cellSize = std::min(kCellSize, std::floor(available / std::max(1, m_count)));
    const qreal row = m_count * m_cellSize;
    const QSize size(snapUp(row + 2 * kPadding, step),
                     snapUp(m_cellSize + 2 * kVerticalPadding, step));
    m_rowX = (size.width() - row) / 2;
    const QPoint position(snapDown(area.x() + (area.width() - size.width()) / 2.0, step),
                          snapDown(area.y() + (area.height() - size.height()) / 2.0, step));
    m_bounds = QRect(position, size);

    const QSize pixels(qRound(size.width() * scale), qRound(size.height() * scale));
    if (m_image.size() != pixels || m_image.devicePixelRatio() != scale) {
        m_window->setRenderTarget({});
        // The upload's format, so no frame converts it.
        m_image = QImage(pixels, QImage::Format_RGBA8888_Premultiplied);
        m_image.setDevicePixelRatio(scale);
        m_image.fill(Qt::transparent);
        m_window->setGeometry(QRect(QPoint(), size));
        QQuickRenderTarget target = QQuickRenderTarget::fromPaintDevice(&m_image);
        target.setDevicePixelRatio(scale);
        m_window->setRenderTarget(target);
        // A window never shown gets no resize event, so nothing sizes its content item.
        m_window->contentItem()->setSize(size);
    }
    m_root->setSize(size);
    m_root->setProperty("darkMode", m_theme && m_theme->darkMode());
    m_root->setProperty("rowX", m_rowX);
    m_root->setProperty("cellSize", m_cellSize);
    m_root->setProperty("iconSize", m_cellSize * kIconRatio);
    m_root->setProperty("entries", entries);
    m_root->setProperty("selectedIndex", selected);
    m_selected = selected;
    m_dirty = true;
}

void SwitcherView::selectWindowCycleEntry(int index)
{
    if (m_root)
        m_root->setProperty("selectedIndex", index);
    m_selected = index;
    // The selection's glass is the node's, not the image's.
    if (m_shown)
        m_panel->notifyChanged();
}

void SwitcherView::hideWindowCycle()
{
    m_shown = false;
    m_panel->setVisible(false);
    if (m_shadow)
        m_shadow->setVisible(false);
}

QRect SwitcherView::selectionBounds() const
{
    if (!m_shown || m_selected < 0 || m_selected >= m_count)
        return {};
    const qreal size = m_cellSize * kSelectionRatio;
    const qreal inset = (m_cellSize - size) / 2;
    return QRectF(m_bounds.x() + m_rowX + m_selected * m_cellSize + inset,
                  m_bounds.y() + rowY() + inset, size, size)
            .toRect();
}

bool SwitcherView::windowCycleViewContains(const QPointF &location) const
{
    return m_shown && QRectF(m_bounds).contains(location);
}

int SwitcherView::windowCycleEntryAt(const QPointF &location) const
{
    if (!windowCycleViewContains(location))
        return -1;
    const QPointF local = location - m_bounds.topLeft() - QPointF(m_rowX, rowY());
    if (local.x() < 0 || local.y() < 0 || local.y() >= m_cellSize)
        return -1;
    const int index = int(local.x() / m_cellSize);
    return index < m_count ? index : -1;
}

QImage SwitcherView::renderImage()
{
    if (!m_shown || !m_root || m_image.isNull())
        return {};
    if (m_dirty)
        render();
    return m_image;
}

void SwitcherView::render()
{
    m_rendering = true;
    m_dirty = false;
    // The software renderer repaints only what changed, over what is there.
    m_control->polishItems();
    m_control->sync();
    m_control->render();
    m_rendering = false;
    ++m_imageGeneration;
}

} // namespace cutefish::switcher
