// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QSettings>
#include <QStringList>

namespace cutefish::input {

// Lists the connected touchscreens by name and keeps which of them the user
// turned off. Exported as com.cutefish.Touchscreen1 for Settings.
class TouchscreenSettings : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.Touchscreen1")
    Q_PROPERTY(QStringList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QStringList disabledDevices READ disabledDevices NOTIFY disabledDevicesChanged)

public:
    explicit TouchscreenSettings(QObject *parent = nullptr, const QString &settingsFile = {});
    ~TouchscreenSettings() override;
    bool registerService();

    QStringList devices() const { return m_devices; }

    // Names, not device nodes, so a choice survives a reboot or a replug.
    // Disabled screens stay listed after they are unplugged.
    QStringList disabledDevices() const { return m_disabledDevices; }

    bool deviceEnabled(const QString &name) const { return !m_disabledDevices.contains(name); }

    void setDevices(const QStringList &devices);

public slots:
    void setDeviceEnabled(const QString &name, bool enabled);

signals:
    void devicesChanged();
    void disabledDevicesChanged();
    void settingsChanged();

private:
    QSettings m_settings;
    QStringList m_devices;
    QStringList m_disabledDevices;
    bool m_registered = false;
};

} // namespace cutefish::input
