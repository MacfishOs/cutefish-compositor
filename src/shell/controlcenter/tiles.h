// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_CONTROLCENTER_TILES_H_
#define CUTEFISH_SHELL_CONTROLCENTER_TILES_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/timer.h"
#include "shell/controlcenter/control_center_services.h"
#include "ui/view.h"

namespace cutefish::ui {
class Icon;
class ImageView;
class Label;
} // namespace cutefish::ui

namespace cutefish::shell {

// An SVG file drawn at its rect's device size on whole device pixels.
class SvgIcon : public ui::View
{
public:
    void setFile(std::string file);
    void setTint(std::optional<ui::Color> tint);

protected:
    void paint(ui::Painter &painter) override;

private:
    std::string m_file;
    std::optional<ui::Color> m_tint;
};

// Header circular button (Lock Screen, Settings)
class HeaderButton : public ui::View
{
public:
    static constexpr double kSize = 32;

    HeaderButton();

    void setIcon(std::string file);

    std::function<void()> onClicked;

    ui::SizeF sizeHint() const override { return {kSize, kSize}; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;
    void pointerPress(const ui::PointerEvent &event) override;
    void pointerRelease(const ui::PointerEvent &event) override;

private:
    SvgIcon *m_icon;
    bool m_pressed = false;
};

// CardItem: represents either a 2x2 Top Card or a Round Quick Toggle.
class CardItem : public ui::View
{
public:
    enum class Mode {
        TopCard,
        QuickToggle,
    };

    static constexpr double kHeight = 60;

    CardItem();
    ~CardItem() override;

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    void setArtwork(std::string name);
    void setGlyph(std::string name);
    void setChecked(bool checked);
    bool isChecked() const { return m_checked; }
    void setActiveColor(ui::Color color);
    void setShowChevron(bool show);
    void setTitle(std::string title);
    void setStatus(std::string status);

    std::function<void()> onClicked;
    std::function<void()> onPressAndHold;
    std::function<void()> onChevronClicked;

    ui::SizeF sizeHint() const override { return {0, kHeight}; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;
    void pointerPress(const ui::PointerEvent &event) override;
    void pointerRelease(const ui::PointerEvent &event) override;

private:
    void updateIcon();

    Mode m_mode = Mode::TopCard;
    std::string m_artwork;
    bool m_checked = false;
    bool m_pressed = false;
    bool m_held = false;
    bool m_showChevron = false;
    ui::Color m_activeColor = ui::Color::rgb(0x007AFF);
    SvgIcon *m_svg;
    SvgIcon *m_chevronSvg;
    ui::Icon *m_glyph;
    ui::Label *m_title;
    ui::Label *m_status;
    base::Timer m_holdTimer;
};

// An icon button for media playback.
class IconButton : public ui::View
{
public:
    IconButton();

    void setFile(std::string file);
    void setSurfaceColor(ui::Color color);

    std::function<void()> onClicked;

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;
    void hoverChanged() override;
    void enabledChanged() override;
    void pointerPress(const ui::PointerEvent &event) override;
    void pointerMove(const ui::PointerEvent &event) override;
    void pointerRelease(const ui::PointerEvent &event) override;

private:
    SvgIcon *m_icon;
    ui::Color m_surface;
    bool m_pressed = false;
    bool m_inside = false;
};

// MPRIS Media Card (76px height)
class MediaTile : public ui::View
{
public:
    static constexpr double kHeight = 76;

    MediaTile();

    void setState(const MediaState &state);
    IconButton *previousButton() const { return m_previous; }
    IconButton *playButton() const { return m_play; }
    IconButton *nextButton() const { return m_next; }

    std::function<void()> onPrevious;
    std::function<void()> onPlay;
    std::function<void()> onPause;
    std::function<void()> onNext;

    ui::SizeF sizeHint() const override { return {0, kHeight}; }

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;

private:
    void updateArtwork();

    MediaState m_state;
    SvgIcon *m_cover;
    ui::ImageView *m_art;
    ui::Label *m_title;
    ui::Label *m_artist;
    IconButton *m_previous;
    IconButton *m_play;
    IconButton *m_next;
    bool m_artReady = false;
};

// DeviceUsersTile (privacy dot applications)
class DeviceUsersTile : public ui::View
{
public:
    static constexpr double kIconSize = 20;

    DeviceUsersTile();

    void setUsers(const std::vector<DeviceUser> &users);
    const std::vector<DeviceUser> &users() const { return m_users; }
    // Takes the sentences again after the session language changed.
    void retranslate();

    double heightForWidth(double width) const override;
    ui::SizeF sizeHint() const override { return {0, heightForWidth(0)}; }

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;

private:
    struct Row {
        ui::View *icon;
        ui::Label *name;
        ui::Label *usage;
        ui::Color dot;
    };

    double rowHeight() const;
    double inset() const;

    std::vector<DeviceUser> m_users;
    std::vector<Row> m_rows;
};

std::string deviceUsageText(const DeviceUser &user);

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_CONTROLCENTER_TILES_H_
