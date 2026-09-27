// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <optional>

#include <QObject>
#include <QSettings>
#include <QStringList>

namespace cutefish::input {

// An input method a desktop file offers, as KDE marks them with
// X-KDE-Wayland-VirtualKeyboard=true.
struct InputMethodProvider {
    QString id;
    QString path;
    QStringList command;
};

QStringList parseDesktopExec(const QString &exec);
QList<InputMethodProvider> findInputMethodProviders(const QStringList &applicationDirs);

class InputMethodSettings : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.InputMethod1")
    Q_PROPERTY(QString inputMethod READ inputMethod NOTIFY inputMethodChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    // Empty arguments select the user's settings file and the XDG application directories.
    explicit InputMethodSettings(QObject *parent = nullptr, const QString &settingsFile = {},
                                 const QStringList &applicationDirs = {});
    ~InputMethodSettings() override;
    bool registerService();

    // The selected desktop file id, or empty when none is selected or installed.
    QString inputMethod() const;

    bool running() const { return m_running; }

    QStringList command() const;
    void setRunning(bool running);
    // Launches `id` for this session only; the saved selection is unchanged.
    void overrideInputMethod(const QString &id);

public slots:
    QStringList availableInputMethods() const;
    bool setInputMethod(const QString &id);

signals:
    void inputMethodChanged();
    void runningChanged();
    void commandChanged();

private:
    std::optional<InputMethodProvider> provider(const QString &id) const;
    QSettings m_settings;
    QStringList m_applicationDirs;
    QString m_selected;
    bool m_running = false;
    bool m_registered = false;
};

} // namespace cutefish::input
