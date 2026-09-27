// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QSettings>

namespace cutefish::input {

class MouseSettings : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.Mouse1")
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool leftHanded READ leftHanded NOTIFY leftHandedChanged)
    Q_PROPERTY(bool naturalScroll READ naturalScroll NOTIFY naturalScrollChanged)
    Q_PROPERTY(bool acceleration READ acceleration NOTIFY accelerationChanged)
    Q_PROPERTY(
            double pointerAcceleration READ pointerAcceleration NOTIFY pointerAccelerationChanged)

public:
    explicit MouseSettings(QObject *parent = nullptr, const QString &settingsFile = {});
    ~MouseSettings() override;
    bool registerService();

    bool available() const { return m_available; }

    bool leftHanded() const { return m_leftHanded; }

    bool naturalScroll() const { return m_naturalScroll; }

    bool acceleration() const { return m_acceleration; }

    double pointerAcceleration() const { return m_pointerAcceleration; }

    void setAvailable(bool available);

public slots:
    void setLeftHanded(bool enabled);
    void setNaturalScroll(bool enabled);
    void setAcceleration(bool enabled);
    void setPointerAcceleration(double value);

signals:
    void availableChanged();
    void leftHandedChanged();
    void naturalScrollChanged();
    void accelerationChanged();
    void pointerAccelerationChanged();
    void settingsChanged();

private:
    void save(const QString &key, const QVariant &value);
    QSettings m_settings;
    bool m_available = false;
    bool m_leftHanded = false;
    bool m_naturalScroll = false;
    bool m_acceleration = true;
    double m_pointerAcceleration = 0;
    bool m_registered = false;
};

} // namespace cutefish::input
