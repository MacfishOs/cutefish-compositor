// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "session/window_debug_service.h"

#include <QDBusError>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <utility>

#include "input/input_manager.h"
#include "scene/window_animation.h"
#include "wm/window.h"
#include "wm/window_frame.h"
#include "wm/window_manager.h"

namespace cutefish::session {

namespace {
const QString serviceName = QStringLiteral("com.cutefish.WindowDebug1");
const QString objectPath = QStringLiteral("/com/cutefish/WindowDebug1");

QJsonArray rectArray(const QRect &rect)
{
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

QJsonObject describe(const wm::Window *window)
{
    QJsonObject object {
            {QStringLiteral("id"), QString::number(window->id())},
            {QStringLiteral("appId"), window->applicationId()},
            {QStringLiteral("title"), window->title()},
            {QStringLiteral("popup"), window->isPopup()},
            {QStringLiteral("role"), QString::fromLatin1(wm::toString(window->role()))},
            {QStringLiteral("state"), QString::fromLatin1(wm::toString(window->stateType()))},
            {QStringLiteral("visible"), window->isVisible()},
            {QStringLiteral("active"), window->isActive()},
            {QStringLiteral("decorated"), window->isDecorated()},
            {QStringLiteral("dragPreview"), window->isDragPreview()},
            {QStringLiteral("bounds"), rectArray(window->bounds())},
            {QStringLiteral("contentBounds"), rectArray(window->contentBounds())},
    };

    if (const auto *animation = dynamic_cast<const scene::WindowAnimation *>(window->node())) {
        object.insert(QStringLiteral("opacity"), animation->opacity());
        object.insert(QStringLiteral("scale"), animation->targetPresentationScale());
    }

    const wm::Insets exterior = window->clientExteriorInsets();
    object.insert(QStringLiteral("clientExteriorInsets"),
                  QJsonArray {exterior.left, exterior.top, exterior.right, exterior.bottom});

    if (const wm::WindowFrame *frame = window->frame()) {
        object.insert(QStringLiteral("titleBar"), rectArray(frame->titleBarBounds()));
        QJsonArray buttons;
        for (const QRect &button : frame->captionButtonBounds())
            buttons.append(rectArray(button));
        object.insert(QStringLiteral("captionButtons"), buttons);
    }
    return object;
}
} // namespace

WindowDebugService::WindowDebugService(wm::WindowManager *manager, QDBusConnection bus)
    : m_manager(manager)
    , m_bus(std::move(bus))
{
}

WindowDebugService::~WindowDebugService()
{
    if (m_registered) {
        m_bus.unregisterService(serviceName);
        m_bus.unregisterObject(objectPath);
    }
}

bool WindowDebugService::registerService()
{
    if (m_registered)
        return true;
    if (!m_bus.registerObject(objectPath, this, QDBusConnection::ExportScriptableSlots)) {
        qWarning() << "Cannot export the window debug service:" << m_bus.lastError().message();
        return false;
    }
    if (!m_bus.registerService(serviceName)) {
        qWarning() << "Cannot own the window debug service:" << m_bus.lastError().message();
        m_bus.unregisterObject(objectPath);
        return false;
    }
    m_registered = true;
    return true;
}

QString WindowDebugService::ListWindows() const
{
    QJsonArray windows;
    for (const wm::Window *window : m_manager->windows())
        windows.append(describe(window));
    return QString::fromUtf8(QJsonDocument(windows).toJson(QJsonDocument::Compact));
}

bool WindowDebugService::MoveWindow(qulonglong id, int x, int y)
{
    wm::Window *window = m_manager->windowById(id);
    if (!window)
        return false;
    window->requestBounds(QRect(QPoint(x, y), window->bounds().size()));
    return true;
}

bool WindowDebugService::SetWindowBounds(qulonglong id, int x, int y, int width, int height)
{
    wm::Window *window = m_manager->windowById(id);
    if (!window)
        return false;
    window->requestBounds(QRect(x, y, width, height));
    return true;
}

bool WindowDebugService::ActivateWindow(qulonglong id)
{
    wm::Window *window = m_manager->windowById(id);
    if (!window)
        return false;
    window->activate();
    return true;
}

bool WindowDebugService::PointerMotion(double x, double y)
{
    if (!m_input)
        return false;
    m_input->onPointerMotionAbsolute(std::chrono::steady_clock::now(), QPointF(x, y));
    return true;
}

bool WindowDebugService::PointerButton(uint button, bool pressed)
{
    if (!m_input)
        return false;
    m_input->onPointerButton(std::chrono::steady_clock::now(), button, pressed);
    return true;
}

bool WindowDebugService::Key(uint code, bool pressed)
{
    if (!m_input)
        return false;
    m_input->onKey(std::chrono::steady_clock::now(), code, pressed);
    return true;
}

} // namespace cutefish::session
