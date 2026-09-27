// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QSettings>

namespace cutefish::input {

// The touchpad half of Chromium's TouchpadSettingsEvdev, plus libinput's
// disable-while-typing and disable-with-an-external-mouse modes. Exported as
// com.cutefish.Touchpad1 for Settings.
class TouchpadSettings : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.Touchpad1")
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    Q_PROPERTY(bool disableWithMouse READ disableWithMouse NOTIFY disableWithMouseChanged)
    Q_PROPERTY(bool disableWhileTyping READ disableWhileTyping NOTIFY disableWhileTypingChanged)
    Q_PROPERTY(bool tapToClick READ tapToClick NOTIFY tapToClickChanged)
    Q_PROPERTY(bool tapDragging READ tapDragging NOTIFY tapDraggingChanged)
    Q_PROPERTY(bool naturalScroll READ naturalScroll NOTIFY naturalScrollChanged)
    Q_PROPERTY(
            double pointerAcceleration READ pointerAcceleration NOTIFY pointerAccelerationChanged)
    Q_PROPERTY(double scrollSpeed READ scrollSpeed NOTIFY scrollSpeedChanged)
    Q_PROPERTY(bool secondaryClickConfigurable READ secondaryClickConfigurable NOTIFY
                       secondaryClickConfigurableChanged)
    Q_PROPERTY(bool twoFingerSecondaryClick READ twoFingerSecondaryClick NOTIFY
                       twoFingerSecondaryClickChanged)

public:
    // libinput reports two-finger scrolling unaccelerated at about 10.5 units
    // per millimetre of finger travel, and clients move content one logical
    // pixel per unit. The factor scales that; the default half puts content
    // under the fingers on a 150% laptop panel.
    static constexpr double kDefaultScrollSpeed = 0.5;
    static constexpr double kMinimumScrollSpeed = 0.1;
    static constexpr double kMaximumScrollSpeed = 2.0;

    explicit TouchpadSettings(QObject *parent = nullptr, const QString &settingsFile = {});
    ~TouchpadSettings() override;
    bool registerService();

    bool available() const { return m_available; }

    bool enabled() const { return m_enabled; }

    bool disableWithMouse() const { return m_disableWithMouse; }

    bool disableWhileTyping() const { return m_disableWhileTyping; }

    bool tapToClick() const { return m_tapToClick; }

    bool tapDragging() const { return m_tapDragging; }

    bool naturalScroll() const { return m_naturalScroll; }

    double pointerAcceleration() const { return m_pointerAcceleration; }

    double scrollSpeed() const { return m_scrollSpeed; }

    // A touchpad offers both two-finger press and bottom-right corner press.
    bool secondaryClickConfigurable() const { return m_secondaryClickConfigurable; }

    bool twoFingerSecondaryClick() const { return m_twoFingerSecondaryClick; }

    void setAvailable(bool available);
    void setSecondaryClickConfigurable(bool configurable);

public slots:
    void setEnabled(bool enabled);
    void setDisableWithMouse(bool enabled);
    void setDisableWhileTyping(bool enabled);
    void setTapToClick(bool enabled);
    void setTapDragging(bool enabled);
    void setNaturalScroll(bool enabled);
    void setPointerAcceleration(double value);
    void setScrollSpeed(double value);
    void setTwoFingerSecondaryClick(bool enabled);

signals:
    void availableChanged();
    void enabledChanged();
    void disableWithMouseChanged();
    void disableWhileTypingChanged();
    void tapToClickChanged();
    void tapDraggingChanged();
    void naturalScrollChanged();
    void pointerAccelerationChanged();
    void scrollSpeedChanged();
    void secondaryClickConfigurableChanged();
    void twoFingerSecondaryClickChanged();
    void settingsChanged();

private:
    void save(const QString &key, const QVariant &value);
    QSettings m_settings;
    bool m_available = false;
    bool m_enabled = true;
    bool m_disableWithMouse = false;
    bool m_disableWhileTyping = true;
    bool m_tapToClick = false;
    bool m_tapDragging = false;
    bool m_naturalScroll = false;
    double m_pointerAcceleration = 0;
    double m_scrollSpeed = kDefaultScrollSpeed;
    bool m_secondaryClickConfigurable = false;
    bool m_twoFingerSecondaryClick = true;
    bool m_registered = false;
};

} // namespace cutefish::input
