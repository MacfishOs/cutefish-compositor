// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/controlcenter/tiles.h"

#include <algorithm>
#include <cmath>

#include "base/url.h"
#include "shell/controlcenter/panel_style.h"
#include "ui/icon.h"
#include "ui/geometry.h"
#include "ui/image_view.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/theme.h"

namespace cutefish::shell {

static constexpr std::chrono::milliseconds kPressAndHold {800};
static constexpr ui::Color kLightArtwork = ui::Color::rgb(0x383838);
static constexpr ui::Color kWhite = ui::Color::rgb(0xFFFFFF);

void SvgIcon::setFile(std::string file)
{
    if (file == m_file)
        return;
    m_file = std::move(file);
    requestPaint();
}

void SvgIcon::setTint(std::optional<ui::Color> tint)
{
    m_tint = tint;
    requestPaint();
}

void SvgIcon::paint(ui::Painter &painter)
{
    if (m_file.empty())
        return;
    const ui::Rect device = painter.snappedRect(localRect());
    const double scale = painter.scale();
    const ui::PointF offset = painter.offset();
    painter.drawSvg(m_file,
                    {device.x / scale - offset.x, device.y / scale - offset.y, device.width / scale,
                     device.height / scale},
                    m_tint);
}

// HeaderButton (32x32 round button for top right header)
HeaderButton::HeaderButton()
{
    m_icon = addChild<SvgIcon>();
}

void HeaderButton::setIcon(std::string file)
{
    m_icon->setFile(std::move(file));
}

void HeaderButton::layout()
{
    m_icon->setGeometry({8, 8, 16, 16});
}

void HeaderButton::paint(ui::Painter &painter)
{
    const ui::RectF rect = localRect();
    const ui::Color bg = m_pressed ? panel::rgba(1, 1, 1, 0.25) : panel::inactiveCircleColor();
    painter.fillRoundedRect(rect, rect.height / 2.0, bg);
}

void HeaderButton::pointerPress(const ui::PointerEvent &event)
{
    if (event.button != ui::MouseButton::Left)
        return;
    m_pressed = true;
    requestPaint();
}

void HeaderButton::pointerRelease(const ui::PointerEvent &event)
{
    if (event.button != ui::MouseButton::Left || !m_pressed)
        return;
    m_pressed = false;
    requestPaint();
    const ui::RectF rect = localRect();
    if (event.position.x >= 0 && event.position.y >= 0 && event.position.x < rect.width
        && event.position.y < rect.height) {
        if (onClicked)
            onClicked();
    }
}

// CardItem
CardItem::CardItem()
{
    m_svg = addChild<SvgIcon>();
    m_glyph = addChild<ui::Icon>(std::string(), 16);
    m_glyph->setVisible(false);
    m_chevronSvg = addChild<SvgIcon>();
    m_chevronSvg->setVisible(false);

    m_title = addChild<ui::Label>();
    m_title->setElide(ui::Label::Elide::Right);

    m_status = addChild<ui::Label>();
    m_status->setElide(ui::Label::Elide::Right);

    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(kPressAndHold);
    m_holdTimer.onTimeout = [this] {
        if (!m_pressed)
            return;
        m_held = true;
        if (onPressAndHold)
            onPressAndHold();
    };
}

CardItem::~CardItem() = default;

void CardItem::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    requestLayout();
    requestPaint();
}

void CardItem::setArtwork(std::string name)
{
    m_artwork = std::move(name);
    m_glyph->setVisible(false);
    m_svg->setVisible(true);
    updateIcon();
}

void CardItem::setGlyph(std::string name)
{
    m_artwork.clear();
    m_glyph->setIconName(std::move(name));
    m_glyph->setVisible(true);
    m_svg->setVisible(false);
    updateIcon();
}

void CardItem::setChecked(bool checked)
{
    if (checked == m_checked)
        return;
    m_checked = checked;
    updateIcon();
    requestPaint();
}

void CardItem::setActiveColor(ui::Color color)
{
    m_activeColor = color;
    requestPaint();
}

void CardItem::setShowChevron(bool show)
{
    m_showChevron = show;
    m_chevronSvg->setVisible(show);
    if (show)
        m_chevronSvg->setFile(panel::artwork("chevron-right", panel::dark()));
    requestLayout();
}

void CardItem::setTitle(std::string title)
{
    if (title != m_title->text()) {
        m_title->setText(std::move(title));
        requestLayout();
    }
}

void CardItem::setStatus(std::string status)
{
    if (status != m_status->text()) {
        m_status->setText(std::move(status));
        requestLayout();
    }
}

void CardItem::updateIcon()
{
    const bool white = panel::dark() || m_checked;
    if (!m_artwork.empty())
        m_svg->setFile(panel::artwork(m_artwork, white));
    m_glyph->setColor(white ? kWhite : kLightArtwork);
}

void CardItem::layout()
{
    const ui::Theme &theme = ui::Theme::current();
    updateIcon();

    if (m_mode == Mode::TopCard) {
        // 2x2 Top Card Layout
        const double plateSize = 40.0;
        const double iconSize = 22.0;
        const ui::RectF plateRect {10, (geometry().height - plateSize) / 2.0, plateSize, plateSize};
        const ui::RectF iconRect {plateRect.x + (plateSize - iconSize) / 2.0,
                                  plateRect.y + (plateSize - iconSize) / 2.0, iconSize, iconSize};

        m_svg->setGeometry(iconRect);
        m_glyph->setGeometry(iconRect);

        m_status->setFontSize(11.0);
        m_status->setColor(panel::secondaryTextColor());
        m_status->setVisible(!m_status->text().empty());

        m_title->setFontSize(12.0);
        m_title->setColor(theme.text);

        const double x = plateRect.x + plateSize + 10.0;
        const double rightSpace = m_showChevron ? 34.0 : 10.0;
        const double width = std::max(0.0, geometry().width - x - rightSpace);
        const double titleHeight = m_title->sizeHint().height;
        const double statusHeight = m_status->isVisible() ? m_status->sizeHint().height : 0.0;
        const double top = (geometry().height - titleHeight - statusHeight) / 2.0;

        m_title->setGeometry({x, top, width, titleHeight});
        m_status->setGeometry({x, top + titleHeight, width, statusHeight});

        if (m_showChevron) {
            m_chevronSvg->setGeometry({geometry().width - 26, (geometry().height - 14) / 2.0, 7, 14});
            m_chevronSvg->setFile(panel::artwork("chevron-right", panel::dark()));
        }
    } else {
        // QuickToggle Layout
        const double circleSize = 42.0;
        const double iconSize = 22.0;
        const double circleX = (geometry().width - circleSize) / 2.0;
        const ui::RectF circleRect {circleX, 0, circleSize, circleSize};
        const ui::RectF iconRect {circleX + (circleSize - iconSize) / 2.0, (circleSize - iconSize) / 2.0,
                                  iconSize, iconSize};

        m_svg->setGeometry(iconRect);
        m_glyph->setGeometry(iconRect);

        m_status->setVisible(false);
        m_title->setFontSize(11.0);
        m_title->setColor(theme.text);
        m_title->setTextAlignment(ui::Label::TextAlignment::Center);
        const double titleHeight = m_title->sizeHint().height;
        m_title->setGeometry({-10.0, 44.0, geometry().width + 20.0, titleHeight});
    }
}

void CardItem::paint(ui::Painter &painter)
{
    if (m_mode == Mode::TopCard) {
        panel::paintTile(painter, localRect());

        const double circleSize = 40.0;
        const ui::RectF slot {10, (geometry().height - circleSize) / 2.0, circleSize, circleSize};
        ui::Color circleBg = m_checked ? m_activeColor : panel::inactiveCircleColor();
        if (m_pressed)
            circleBg = panel::shade(circleBg, 0.15);
        painter.fillRoundedRect(slot, circleSize / 2.0, circleBg);
    } else {
        // QuickToggle circle button
        const double circleSize = 42.0;
        const double circleX = (geometry().width - circleSize) / 2.0;
        const ui::RectF circleRect {circleX, 0, circleSize, circleSize};
        ui::Color circleBg = m_checked ? m_activeColor : panel::inactiveCircleColor();
        if (m_pressed)
            circleBg = panel::shade(circleBg, 0.15);
        painter.fillRoundedRect(circleRect, circleSize / 2.0, circleBg);
    }
}

void CardItem::pointerPress(const ui::PointerEvent &event)
{
    if (event.button != ui::MouseButton::Left)
        return;
    m_pressed = true;
    m_held = false;
    m_holdTimer.start();
    requestPaint();
}

void CardItem::pointerRelease(const ui::PointerEvent &event)
{
    if (event.button != ui::MouseButton::Left || !m_pressed)
        return;
    m_pressed = false;
    m_holdTimer.stop();
    requestPaint();

    const ui::RectF rect = localRect();
    const bool inside = event.position.x >= 0 && event.position.y >= 0 && event.position.x < rect.width
            && event.position.y < rect.height;

    if (inside && !m_held) {
        if (m_mode == Mode::TopCard && m_showChevron && event.position.x > rect.width - 36) {
            if (onChevronClicked)
                onChevronClicked();
            else if (onPressAndHold)
                onPressAndHold();
        } else {
            if (onClicked)
                onClicked();
        }
    }
}

IconButton::IconButton()
    : m_surface(ui::Theme::current().background)
{
    m_icon = addChild<SvgIcon>();
    setTracksHover(true);
}

void IconButton::setFile(std::string file)
{
    m_icon->setFile(std::move(file));
}

void IconButton::setSurfaceColor(ui::Color color)
{
    m_surface = color;
    requestPaint();
}

void IconButton::layout()
{
    const double size = geometry().height * 0.75;
    m_icon->setGeometry({(geometry().width - size) / 2, (geometry().height - size) / 2, size, size});
}

void IconButton::paint(ui::Painter &painter)
{
    if (!isEnabled() || !isHovered())
        return;
    const bool dark = panel::isDark(m_surface);
    const bool pressed = m_pressed && m_inside;
    const ui::Color color = panel::on(m_surface, pressed ? (dark ? 0.3 : 0.2) : (dark ? 0.2 : 0.1));
    const ui::RectF rect = localRect();
    painter.fillRoundedRect(rect, rect.height / 2, color);
}

void IconButton::hoverChanged()
{
    requestPaint();
}

void IconButton::enabledChanged()
{
    if (!isEnabled())
        m_pressed = false;
    requestPaint();
}

void IconButton::pointerPress(const ui::PointerEvent &event)
{
    if (event.button != ui::MouseButton::Left || !isEnabled())
        return;
    m_pressed = true;
    m_inside = true;
    requestPaint();
}

void IconButton::pointerMove(const ui::PointerEvent &event)
{
    if (!m_pressed)
        return;
    const ui::RectF rect = localRect();
    const bool inside = event.position.x >= 0 && event.position.y >= 0 && event.position.x < rect.width
            && event.position.y < rect.height;
    if (inside != m_inside) {
        m_inside = inside;
        requestPaint();
    }
}

void IconButton::pointerRelease(const ui::PointerEvent &event)
{
    if (event.button != ui::MouseButton::Left || !m_pressed)
        return;
    pointerMove(event);
    m_pressed = false;
    requestPaint();
    if (m_inside && isEnabled() && onClicked)
        onClicked();
}

// MediaTile (76px height)
MediaTile::MediaTile()
{
    m_cover = addChild<SvgIcon>();
    m_art = addChild<ui::ImageView>();
    m_art->setFillMode(ui::ImageView::FillMode::PreserveAspectFit);
    m_art->setCornerRadius(12, ui::Corners::Continuous);
    m_art->setVisible(false);
    m_art->onStatusChanged = [this](ui::ImageView::Status status) {
        m_artReady = status == ui::ImageView::Status::Ready;
        m_art->setVisible(m_artReady);
        m_cover->setVisible(!m_artReady);
        requestPaint();
    };
    m_title = addChild<ui::Label>();
    m_title->setElide(ui::Label::Elide::Right);
    m_artist = addChild<ui::Label>();
    m_artist->setElide(ui::Label::Elide::Right);
    m_artist->setOpacity(0.6);
    m_previous = addChild<IconButton>();
    m_play = addChild<IconButton>();
    m_next = addChild<IconButton>();
    m_previous->onClicked = [this] {
        if (onPrevious)
            onPrevious();
    };
    m_play->onClicked = [this] {
        if (m_state.playing ? bool(onPause) : bool(onPlay))
            (m_state.playing ? onPause : onPlay)();
    };
    m_next->onClicked = [this] {
        if (onNext)
            onNext();
    };
    setState({});
}

void MediaTile::setState(const MediaState &state)
{
    m_state = state;
    const bool control = state.hasPlayer && state.canControl;
    m_previous->setEnabled(control && state.canGoPrevious);
    m_play->setEnabled(control && (state.playing ? state.canPause : state.canPlay));
    m_next->setEnabled(control && state.canGoNext);
    for (IconButton *button : {m_previous, m_play, m_next})
        button->setOpacity(button->isEnabled() ? 1 : 0.35);
    std::string title = "Not Playing";
    if (state.hasPlayer)
        title = !state.title.empty() ? state.title : state.identity;
    m_title->setText(std::move(title));
    m_artist->setText(state.hasPlayer ? state.artist : std::string());
    updateArtwork();
    requestLayout();
}

void MediaTile::updateArtwork()
{
    const bool white = panel::dark();
    m_cover->setFile(panel::artwork("media-cover", white));
    m_previous->setFile(panel::artwork("media-skip-backward-symbolic", white));
    m_play->setFile(panel::artwork(m_state.playing ? "media-playback-pause-symbolic"
                                                   : "media-playback-start-symbolic",
                                   white));
    m_next->setFile(panel::artwork("media-skip-forward-symbolic", white));
    for (IconButton *button : {m_previous, m_play, m_next})
        button->setSurfaceColor(panel::tileInkColor());

    std::string path;
    if (m_state.hasPlayer && !m_state.artUrl.empty()) {
        if (auto local = base::url::toLocalFile(m_state.artUrl))
            path = *local;
        else if (m_state.artUrl.starts_with("/"))
            path = m_state.artUrl;
        else if (m_state.artUrl.starts_with("file://")) {
            std::string url = m_state.artUrl.substr(7);
            if (url.starts_with("localhost/"))
                url = url.substr(10);
            path = base::url::percentDecoded(url);
        } else
            path = m_state.artUrl;
    }
    if (path != m_art->source()) {
        m_artReady = false;
        m_art->setVisible(false);
        m_cover->setVisible(true);
        m_art->setSource(path);
    }
}

void MediaTile::layout()
{
    updateArtwork();
    const double height = geometry().height;
    const double padding = 10.0;
    const double artSize = 56.0;
    const ui::RectF art {padding, (height - artSize) / 2.0, artSize, artSize};
    m_cover->setGeometry(art);
    m_art->setGeometry(art);
    m_art->setSourceSize({artSize, artSize});

    const double buttonSize = 28.0;
    const double spacing = 6.0;
    const double buttonsWidth = 3 * buttonSize + 2 * spacing;
    const double buttonsX = geometry().width - padding - buttonsWidth;
    const double buttonY = (height - buttonSize) / 2.0;

    m_previous->setGeometry({buttonsX, buttonY, buttonSize, buttonSize});
    m_play->setGeometry({buttonsX + buttonSize + spacing, buttonY, buttonSize, buttonSize});
    m_next->setGeometry({buttonsX + 2 * (buttonSize + spacing), buttonY, buttonSize, buttonSize});

    m_artist->setFontSize(11.0);
    m_artist->setVisible(!m_artist->text().empty());
    m_title->setFontSize(13.0);

    const double textX = art.x + art.width + 12.0;
    const double textWidth = std::max(0.0, buttonsX - 8.0 - textX);
    const double titleHeight = m_title->sizeHint().height;
    const double artistHeight = m_artist->isVisible() ? m_artist->sizeHint().height : 0.0;
    const double textTop = (height - titleHeight - artistHeight) / 2.0;

    m_title->setGeometry({textX, textTop, textWidth, titleHeight});
    m_artist->setGeometry({textX, textTop + titleHeight, textWidth, artistHeight});
}

void MediaTile::paint(ui::Painter &painter)
{
    panel::paintTile(painter, localRect());
    if (m_artReady)
        return;
    const ui::Color surface = panel::tileInkColor();
    const ui::RectF art = m_cover->geometry();
    painter.fillRoundedRect(art, 12, panel::on(surface, panel::isDark(surface) ? 0.15 : 0.1),
                            ui::Corners::Continuous);
}

std::string deviceUsageText(const DeviceUser &user)
{
    if (user.camera && user.microphone && user.systemAudio)
        return "is using the camera and microphone and recording system audio";
    if (user.camera && user.microphone)
        return "is using the camera and microphone";
    if (user.camera && user.systemAudio)
        return "is using the camera and recording system audio";
    if (user.microphone && user.systemAudio)
        return "is using the microphone and recording system audio";
    if (user.camera)
        return "is using the camera";
    if (user.microphone)
        return "is using the microphone";
    return "is recording system audio";
}

DeviceUsersTile::DeviceUsersTile() = default;

void DeviceUsersTile::setUsers(const std::vector<DeviceUser> &users)
{
    if (users == m_users)
        return;
    m_users = users;
    for (Row &row : m_rows) {
        takeChild(row.icon);
        takeChild(row.name);
        takeChild(row.usage);
    }
    m_rows.clear();
    for (const DeviceUser &user : m_users) {
        Row row;
        row.icon = addChild<ui::ThemeIcon>(user.icon.empty() ? "application-x-executable" : user.icon,
                                           kIconSize);
        row.name = addChild<ui::Label>(user.name);
        row.name->setElide(ui::Label::Elide::Right);
        row.usage = addChild<ui::Label>(" " + deviceUsageText(user));
        row.dot = user.camera ? ui::accent::green : user.microphone ? ui::accent::orange : ui::accent::purple;
        m_rows.push_back(row);
    }
    requestLayout();
}

double DeviceUsersTile::inset() const
{
    return 6.0 + (40.0 - kIconSize) / 2;
}

double DeviceUsersTile::rowHeight() const
{
    double height = kIconSize;
    for (const Row &row : m_rows)
        height = std::max(height, row.name->sizeHint().height);
    return height;
}

double DeviceUsersTile::heightForWidth(double) const
{
    if (m_rows.empty())
        return 0;
    const double spacing = ui::Theme::current().smallSpacing * 1.5;
    return 2 * inset() + m_rows.size() * rowHeight() + (m_rows.size() - 1) * spacing;
}

void DeviceUsersTile::layout()
{
    const ui::Theme &theme = ui::Theme::current();
    const double spacing = theme.smallSpacing * 1.5;
    const double left = theme.largeSpacing;
    const double right = geometry().width - inset();
    const double height = rowHeight();
    double y = inset();
    for (const Row &row : m_rows) {
        row.icon->setGeometry({left + (40.0 - kIconSize) / 2, y + (height - kIconSize) / 2, kIconSize, kIconSize});
        const double sentenceX = left + 40.0 + spacing;
        const double sentenceWidth = std::max(0.0, right - 6 - spacing - sentenceX);
        const double usageWidth = row.usage->sizeHint().width;
        const double nameWidth = std::min(row.name->sizeHint().width, std::max(0.0, sentenceWidth - usageWidth));
        const double nameHeight = row.name->sizeHint().height;
        row.name->setGeometry({sentenceX, y + (height - nameHeight) / 2, nameWidth, nameHeight});
        row.usage->setGeometry({sentenceX + nameWidth, y + (height - nameHeight) / 2, usageWidth, nameHeight});
        y += height + spacing;
    }
}

void DeviceUsersTile::paint(ui::Painter &painter)
{
    panel::paintTile(painter, localRect());
    const double spacing = ui::Theme::current().smallSpacing * 1.5;
    const double height = rowHeight();
    double y = inset();
    for (const Row &row : m_rows) {
        painter.fillRoundedRect({geometry().width - inset() - 6, y + (height - 6) / 2, 6, 6}, 3, row.dot);
        y += height + spacing;
    }
}

} // namespace cutefish::shell
