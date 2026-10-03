// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "decoration/decoration_node.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "decoration/caption_button.h"
#include "decoration/decoration.h"
#include "decoration/decoration_theme.h"
#include "decoration/text.h"
#include "wm/window.h"

namespace cutefish::decoration {

namespace {

QString colorKey(const render::Color& color)
{
    return QStringLiteral("%1,%2,%3,%4")
        .arg(color.r, 0, 'f', 3)
        .arg(color.g, 0, 'f', 3)
        .arg(color.b, 0, 'f', 3)
        .arg(color.a, 0, 'f', 3);
}

const char* iconName(CaptionButtonType type, bool restore)
{
    switch (type) {
    case CaptionButtonType::Minimize:
        return "minimize";
    case CaptionButtonType::Maximize:
        return restore ? "restore" : "maximize";
    case CaptionButtonType::Close:
        return "close";
    }
    return "";
}

} // namespace

DecorationNode::DecorationNode(const Decoration* decoration, Theme* theme)
    : scene::Node(QStringLiteral("decoration"))
    , m_decoration(decoration)
    , m_theme(theme)
{
}

DecorationNode::~DecorationNode() = default;

void DecorationNode::paint(render::Renderer& renderer, const QRect& absoluteGeometry) const
{
    if (!m_decoration || !m_decoration->window() || absoluteGeometry.isEmpty()
        || !renderer.drawsInto(absoluteGeometry))
        return;
    paintFrame(renderer, absoluteGeometry);
    paintCaption(renderer, absoluteGeometry);
    paintButtons(renderer, absoluteGeometry);
}

void DecorationNode::paintFrame(render::Renderer& renderer, const QRect& bounds) const
{
    const auto insets = m_decoration->window()->frameInsets();
    const auto& color = m_theme->palette().titleBar;
    auto frameColor = m_decoration->isActive()
        ? m_theme->visualStyle().activeOutline : m_theme->visualStyle().inactiveOutline;
    frameColor.a = 1;
    const int top = std::min(bounds.height(), insets.top);
    const int bottom = std::min(bounds.height() - top, insets.bottom);
    renderer.fillRect(QRect(bounds.x(), bounds.y(), bounds.width(), top), color);
    renderer.fillRect(QRect(bounds.x(), bounds.y() + top, insets.left,
                            bounds.height() - top - bottom), frameColor);
    renderer.fillRect(QRect(bounds.x() + bounds.width() - insets.right, bounds.y() + top,
                            insets.right, bounds.height() - top - bottom), frameColor);
    renderer.fillRect(QRect(bounds.x(), bounds.y() + bounds.height() - bottom,
                            bounds.width(), bottom), frameColor);
}

void DecorationNode::paintCaption(render::Renderer& renderer, const QRect& bounds) const
{
    const Metrics& metrics = m_theme->metrics();
    const QRect area = m_decoration->captionArea().translated(bounds.topLeft());
    if (area.width() <= 0 || m_decoration->title().isEmpty())
        return;

    const render::Color color = m_theme->captionColor(m_decoration->isActive());
    const double scale = renderer.rasterScale();
    const QString key = m_decoration->title() + QLatin1Char('\n') + colorKey(color) +
        QStringLiteral("\n%1:%2").arg(metrics.textPixelSize).arg(area.width());
    auto caption = std::find_if(m_captions.begin(), m_captions.end(), [&](const Caption& entry) {
        return entry.scale == scale;
    });
    if (caption == m_captions.end() || caption->key != key) {
        const QString text = m_theme->textRenderer().elideText(m_decoration->title(),
                                                               metrics.textPixelSize, area.width());
        const Image image =
            m_theme->textRenderer().renderText(text, metrics.textPixelSize, color, scale);
        if (image.size().isEmpty())
            return;
        // Padded to the pixels its whole-unit target covers, so every texel
        // lands on one physical pixel instead of being resampled.
        const QSize logical(int(std::ceil(image.size().width() / scale)),
                            int(std::ceil(image.size().height() / scale)));
        const QSize pixels(qRound(logical.width() * scale), qRound(logical.height() * scale));
        std::vector<uint8_t> padded(size_t(pixels.width()) * pixels.height() * 4, 0);
        for (int row = 0; row < image.size().height(); ++row) {
            std::memcpy(padded.data() + size_t(row) * pixels.width() * 4,
                        image.bits() + size_t(row) * image.stride(), size_t(image.stride()));
        }
        auto texture = renderer.createTexture(pixels, padded.data(), pixels.width() * 4,
                                              render::PixelFormat::Rgba8888);
        if (!texture)
            return;
        if (caption == m_captions.end()) {
            // Two scales cover a mixed pair of outputs; a third is a scale change.
            if (m_captions.size() >= 2)
                m_captions.erase(m_captions.begin());
            caption = m_captions.insert(m_captions.end(), Caption {scale});
        }
        caption->key = key;
        caption->texture = std::move(texture);
    }
    render::Texture* texture = caption->texture.get();

    // Centred in the titlebar, and moved into the space beside the buttons when
    // centring would put it under them: the rule the Cutefish KWin decoration
    // used, which keeps the caption off the buttons without ever centring it in
    // a box that is not the window.
    const QSize size(qRound(texture->size().width() / scale),
                     qRound(texture->size().height() / scale));
    QRect target(QPoint(0, 0), size);
    target.moveLeft(bounds.left() + (bounds.width() - size.width()) / 2);
    if (target.left() < area.left())
        target.moveLeft(area.left());
    else if (target.right() > area.right())
        target.moveRight(area.right());
    target.moveTop(bounds.top()
                   + (std::min(bounds.height(), metrics.titleBarHeight) - size.height()) / 2);
    // Mapped, so the texels land on whole physical pixels.
    renderer.drawTextureMapped(texture, target, 1.0f, QTransform());
}

void DecorationNode::paintButtons(render::Renderer& renderer, const QRect& bounds) const
{
    const int rasterDensity = int(std::ceil(renderer.rasterScale()));
    const Metrics& metrics = m_theme->metrics();
    const Palette& palette = m_theme->palette();
    const render::Color iconColor = palette.buttonIcon;

    for (const CaptionButton& button : m_decoration->buttons()) {
        const QRect geometry = button.geometry().translated(bounds.topLeft());
        if (!bounds.contains(geometry.center()))
            continue;

        if (button.isHovered() || button.isPressed()) {
            const render::Color wash =
                button.isPressed() ? palette.buttonPressed : palette.buttonHover;
            const QString key = (QString::number(rasterDensity) + QStringLiteral("wash:%1:%2"))
                                    .arg(metrics.buttonSize)
                                    .arg(colorKey(wash));
            render::Texture* texture = m_theme->texture(renderer, key, [&] {
                return renderButtonHighlight(metrics.buttonSize * rasterDensity, wash);
            });
            if (texture) {
                QRect target(QPoint(0, 0), (QSizeF(texture->size()) / rasterDensity).toSize());
                target.moveCenter(geometry.center());
                renderer.drawTexture(texture, target);
            }
        }

        const bool restore = m_decoration->isRestoreButton();
        const QString key = (QString::number(rasterDensity) + QStringLiteral("icon:%1:%2:%3"))
                                .arg(QLatin1String(iconName(button.type(), restore)))
                                .arg(metrics.iconSize)
                                .arg(colorKey(iconColor));
        render::Texture* texture = m_theme->texture(renderer, key, [&] {
            return renderCaptionIcon(button.type(), restore, metrics.iconSize * rasterDensity,
                                     iconColor);
        });
        if (!texture)
            continue;
        QRect target(QPoint(0, 0), (QSizeF(texture->size()) / rasterDensity).toSize());
        target.moveCenter(geometry.center());
        renderer.drawTexture(texture, target);
    }
}

} // namespace cutefish::decoration
