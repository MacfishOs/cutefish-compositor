// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "decoration/caption_button.h"

#include <cmath>

#include "ui/bitmap.h"
#include "ui/bitmap_painter.h"
#include "ui/path.h"

namespace cutefish::decoration {

CaptionButton::CaptionButton(CaptionButtonType type)
    : m_type(type)
{
}

bool CaptionButton::setHovered(bool hovered)
{
    if (hovered == m_hovered)
        return false;
    m_hovered = hovered;
    return true;
}

bool CaptionButton::setPressed(bool pressed)
{
    if (pressed == m_pressed)
        return false;
    m_pressed = pressed;
    return true;
}

Image renderCaptionIcon(CaptionButtonType type, bool restore, int size,
                        const render::Color& color)
{
    Image image(base::Size(size, size));
    if (size <= 0)
        return image;

    ui::Bitmap mask(size, size, ui::Bitmap::Format::Argb32Premultiplied);
    mask.fill(0);
    {
        ui::BitmapPainter painter(mask);
        painter.setAntialiasing(true);
        painter.scale(size / 24.0, size / 24.0);

        const ui::Color white = ui::Color::rgb(0xFFFFFF);
        if (type == CaptionButtonType::Close) {
            // macOS diagonal cross - bold, crisp and rounded
            painter.drawLine({8.0, 8.0}, {16.0, 16.0}, white, 2.1, ui::LineCap::Round);
            painter.drawLine({16.0, 8.0}, {8.0, 16.0}, white, 2.1, ui::LineCap::Round);
        } else if (type == CaptionButtonType::Minimize) {
            // macOS horizontal minus bar - bold, crisp and rounded
            painter.drawLine({7.0, 12.0}, {17.0, 12.0}, white, 2.1, ui::LineCap::Round);
        } else if (!restore) {
            // macOS Fullscreen (two diagonal opposing solid triangles)
            ui::Path tr;
            tr.moveTo(9.5, 7.0);
            tr.lineTo(17.0, 7.0);
            tr.lineTo(17.0, 14.5);
            tr.close();
            painter.fillPath(tr, white);
            painter.strokePath(tr, white, 0.8, ui::LineCap::Round, ui::LineJoin::Round);

            ui::Path bl;
            bl.moveTo(7.0, 9.5);
            bl.lineTo(7.0, 17.0);
            bl.lineTo(14.5, 17.0);
            bl.close();
            painter.fillPath(bl, white);
            painter.strokePath(bl, white, 0.8, ui::LineCap::Round, ui::LineJoin::Round);
        } else {
            // macOS Exit Fullscreen / Restore (two diagonal inward solid triangles)
            ui::Path tr;
            tr.moveTo(13.5, 6.0);
            tr.lineTo(13.5, 10.5);
            tr.lineTo(18.0, 10.5);
            tr.close();
            painter.fillPath(tr, white);
            painter.strokePath(tr, white, 0.8, ui::LineCap::Round, ui::LineJoin::Round);

            ui::Path bl;
            bl.moveTo(10.5, 18.0);
            bl.lineTo(10.5, 13.5);
            bl.lineTo(6.0, 13.5);
            bl.close();
            painter.fillPath(bl, white);
            painter.strokePath(bl, white, 0.8, ui::LineCap::Round, ui::LineJoin::Round);
        }
    }
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x)
            image.blend(x, y, color, mask.alpha(x, y) / 255.0f);
    }
    return image;
}

Image renderButtonHighlight(int buttonSize, const render::Color& color)
{
    const int margin = (buttonSize % 14 == 0)
        ? 0
        : (buttonSize > 16 ? static_cast<int>(std::lround(buttonSize * 0.1)) : 0);
    const int diameter = buttonSize - 2 * margin;
    Image image(base::Size(diameter, diameter));
    if (diameter <= 0)
        return image;
    fillRoundedRect(image, base::RectF(0, 0, diameter, diameter), diameter / 2.0, color);
    return image;
}

} // namespace cutefish::decoration
