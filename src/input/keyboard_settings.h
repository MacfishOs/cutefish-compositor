// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <functional>

#include <QObject>
#include <QSettings>

#include "input/types.h"

namespace cutefish::input {

class KeyboardSettings : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.Keyboard1")
    Q_PROPERTY(bool repeatEnabled READ repeatEnabled NOTIFY repeatEnabledChanged)
    Q_PROPERTY(int repeatDelay READ repeatDelay NOTIFY repeatDelayChanged)
    Q_PROPERTY(int repeatInterval READ repeatInterval NOTIFY repeatIntervalChanged)
    Q_PROPERTY(QString layout READ layout NOTIFY layoutChanged)
    Q_PROPERTY(QString shortcutConvention READ shortcutConventionName
                       NOTIFY shortcutConventionChanged)

public:
    static constexpr int kMinRepeatDelayMs = 150;
    static constexpr int kMaxRepeatDelayMs = 2000;
    static constexpr int kMinRepeatIntervalMs = 20;
    static constexpr int kMaxRepeatIntervalMs = 2000;

    // Compiles and installs a layout name; false when it cannot be used.
    using LayoutApplier = std::function<bool(const QString &)>;

    explicit KeyboardSettings(QObject *parent = nullptr, const QString &settingsFile = {});
    ~KeyboardSettings() override;
    bool registerService();

    // Installs the saved layout right away. A saved layout that no longer
    // compiles falls back to the default.
    void setLayoutApplier(LayoutApplier applier);

    bool repeatEnabled() const { return m_repeatEnabled; }

    int repeatDelay() const { return m_repeatDelay; }

    int repeatInterval() const { return m_repeatInterval; }

    // "layout" or "layout-variant", e.g. "de-neo"; the xkb default when the
    // user never picked one.
    QString layout() const;

    ShortcutConvention shortcutConvention() const { return m_shortcutConvention; }

    // "pc" or "command"; see ShortcutConvention.
    QString shortcutConventionName() const;

public slots:
    void setRepeatEnabled(bool enabled);
    void setRepeatDelay(int milliseconds);
    void setRepeatInterval(int milliseconds);
    // An empty name goes back to the default.
    bool setLayout(const QString &name);
    // False for a name that is not a convention.
    bool setShortcutConvention(const QString &name);

signals:
    void repeatEnabledChanged();
    void repeatDelayChanged();
    void repeatIntervalChanged();
    void layoutChanged();
    void shortcutConventionChanged();

private:
    void save(const QString &key, const QVariant &value);
    QSettings m_settings;
    LayoutApplier m_applier;
    bool m_repeatEnabled = true;
    int m_repeatDelay = 0;
    int m_repeatInterval = 0;
    QString m_layout;
    ShortcutConvention m_shortcutConvention = ShortcutConvention::Pc;
    bool m_registered = false;
};

} // namespace cutefish::input
