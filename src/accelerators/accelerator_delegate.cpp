// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "accelerators/accelerator_delegate.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>

namespace cutefish::accelerators {

namespace {

void callScreenshot(const QString &method, const QVariantList &arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("com.cutefish.Screenshot"), QStringLiteral("/Screenshot"),
            QStringLiteral("com.cutefish.Screenshot"), method);
    message.setArguments(arguments);
    QDBusConnection::sessionBus().asyncCall(message);
}

void callOsd(const QString &method)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("com.cutefish.Osd"), QStringLiteral("/Osd"),
            QStringLiteral("com.cutefish.Osd"), method);
    QDBusConnection::sessionBus().asyncCall(message);
}

} // namespace

void DBusAcceleratorDelegate::toggleAppList()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("com.cutefish.Launcher"), QStringLiteral("/Launcher"),
            QStringLiteral("com.cutefish.Launcher"), QStringLiteral("toggle"));
    QDBusConnection::sessionBus().asyncCall(message);
}

void DBusAcceleratorDelegate::takeScreenshot(bool toClipboard)
{
    callScreenshot(QStringLiteral("takeScreenshot"), {toClipboard});
}

void DBusAcceleratorDelegate::takePartialScreenshot(bool toClipboard)
{
    callScreenshot(QStringLiteral("takePartialScreenshot"), {toClipboard});
}

void DBusAcceleratorDelegate::startCaptureMode()
{
    callScreenshot(QStringLiteral("startCaptureMode"));
}

void DBusAcceleratorDelegate::stopScreenRecording()
{
    callScreenshot(QStringLiteral("stopRecording"));
}

void DBusAcceleratorDelegate::toggleClipboardHistory()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("com.cutefish.ClipboardHistory"), QStringLiteral("/ClipboardHistory"),
            QStringLiteral("com.cutefish.ClipboardHistory"), QStringLiteral("toggle"));
    QDBusConnection::sessionBus().asyncCall(message);
}

void DBusAcceleratorDelegate::volumeDown()
{
    callOsd(QStringLiteral("volumeDown"));
}

void DBusAcceleratorDelegate::volumeUp()
{
    callOsd(QStringLiteral("volumeUp"));
}

void DBusAcceleratorDelegate::toggleMute()
{
    callOsd(QStringLiteral("toggleMute"));
}

void DBusAcceleratorDelegate::toggleMicrophoneMute()
{
    callOsd(QStringLiteral("toggleMicrophoneMute"));
}

void DBusAcceleratorDelegate::brightnessDown()
{
    callOsd(QStringLiteral("brightnessDown"));
}

void DBusAcceleratorDelegate::brightnessUp()
{
    callOsd(QStringLiteral("brightnessUp"));
}

void DBusAcceleratorDelegate::keyboardBrightnessDown()
{
    callOsd(QStringLiteral("keyboardBrightnessDown"));
}

void DBusAcceleratorDelegate::keyboardBrightnessUp()
{
    callOsd(QStringLiteral("keyboardBrightnessUp"));
}

} // namespace cutefish::accelerators
