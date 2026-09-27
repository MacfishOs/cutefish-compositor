// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_FALLBACK_PANEL_H
#define CUTEFISH_AUTH_FALLBACK_PANEL_H

#include <QObject>
#include <QString>

class QPainter;
class QSize;

namespace cutefish::auth {

class AuthController;

// The lock screen drawn with QPainter when its QML fails to load, so a broken
// Qt install still leaves the password prompt rather than a locked wallpaper.
class FallbackPanel : public QObject
{
    Q_OBJECT
public:
    explicit FallbackPanel(AuthController *controller, QObject *parent = nullptr);
    ~FallbackPanel() override;

    // A key press, as its keysym and the text it types.
    void key(uint32_t symbol, const QString &text);

    void paint(QPainter &painter, const QSize &size) const;

    qsizetype typedLength() const { return m_typed.size(); }

signals:
    void changed();

private:
    void clearTyped();

    AuthController *m_controller;
    QString m_typed;
};

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_FALLBACK_PANEL_H
