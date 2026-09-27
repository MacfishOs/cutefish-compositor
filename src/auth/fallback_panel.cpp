// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/fallback_panel.h"
#include "auth/auth_controller.h"
#include "auth/secret.h"
#include <QPainter>
#include <algorithm>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace cutefish::auth {
namespace {
constexpr qsizetype kMaxLength = 1024;
constexpr int kColumnWidth = 288;
constexpr int kFieldHeight = 38;
constexpr int kSpacing = 18;
} // namespace

FallbackPanel::FallbackPanel(AuthController *controller, QObject *parent)
    : QObject(parent)
    , m_controller(controller)
{
    m_typed.reserve(kMaxLength);
    connect(m_controller, &AuthController::authenticationFailed, this, [this] {
        clearTyped();
        emit changed();
    });
    connect(m_controller, &AuthController::stateChanged, this, &FallbackPanel::changed);
    connect(m_controller, &AuthController::promptChanged, this, &FallbackPanel::changed);
    connect(m_controller, &AuthController::userChanged, this, &FallbackPanel::changed);
}

FallbackPanel::~FallbackPanel()
{
    clearTyped();
}

void FallbackPanel::clearTyped()
{
    wipeString(m_typed);
    m_typed.reserve(kMaxLength);
}

void FallbackPanel::key(uint32_t symbol, const QString &text)
{
    if (m_controller->busy() || m_controller->succeeded())
        return;
    switch (symbol) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (!m_typed.isEmpty())
            m_controller->authenticate(std::exchange(m_typed, QString()));
        m_typed.reserve(kMaxLength);
        break;
    case XKB_KEY_BackSpace:
        if (!m_typed.isEmpty()) {
            m_typed[m_typed.size() - 1] = QChar(u'\0');
            m_typed.chop(1);
        }
        break;
    case XKB_KEY_Escape:
        clearTyped();
        break;
    default:
        if (text.isEmpty() || m_typed.size() + text.size() > kMaxLength)
            return;
        if (m_controller->failed())
            m_controller->reset();
        m_typed.append(text);
        break;
    }
    emit changed();
}

void FallbackPanel::paint(QPainter &painter, const QSize &size) const
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const int width = std::min(size.width() - 48, kColumnWidth);
    const int x = (size.width() - width) / 2;
    int y = size.height() / 2 - 64;
    QFont font = painter.font();

    font.setPixelSize(17);
    font.setWeight(QFont::Medium);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QString name = m_controller->displayName().isEmpty() ? m_controller->userName()
                                                              : m_controller->displayName();
    painter.drawText(QRect(x, y, width, 24), Qt::AlignCenter, name);
    y += 24 + kSpacing;

    const QRectF field(x + 0.5, y + 0.5, width - 1, kFieldHeight - 1);
    painter.setPen(QPen(QColor(255, 255, 255, 128), 1));
    painter.setBrush(QColor(255, 255, 255, 66));
    painter.drawRoundedRect(field, field.height() / 2, field.height() / 2);
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    painter.setFont(font);
    QString shown;
    if (m_typed.isEmpty()) {
        shown = m_controller->prompt().isEmpty() ? tr("Password") : m_controller->prompt();
        painter.setPen(QColor(255, 255, 255, 184));
    } else {
        shown = QString(std::min<qsizetype>(m_typed.size(), 24), QChar(u'•'));
        painter.setPen(Qt::white);
    }
    painter.drawText(field.adjusted(16, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, shown);
    y += kFieldHeight + kSpacing;

    font.setPixelSize(13);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255, 184));
    QString message;
    if (m_controller->failed())
        message = m_controller->errorMessage();
    else if (m_controller->busy())
        message = tr("Checking…");
    else
        message = tr("The lock screen could not be loaded. Type your password and press Enter.");
    painter.drawText(QRect(x, y, width, 64), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                     message);
}

} // namespace cutefish::auth
