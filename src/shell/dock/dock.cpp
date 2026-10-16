// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/dock/dock.h"

#include <tuple>

#include "base/logging.h"
#include "shell/common/file_thread.h"
#include "shell/common/task_manager.h"

namespace cutefish::shell {

namespace dbus = base::dbus;

static const base::LogCategory cfDock("cutefish.shell.dock");

Dock::Dock(dbus::Connection *bus, TaskManager &tasks, ApplicationLauncher &launcher, std::string settingsPath,
           std::unique_ptr<TrashMonitor> trash)
    : m_bus(bus)
    , m_settings(settingsPath)
    , m_applications(tasks, launcher, settingsPath)
    , m_minimizedWindows(tasks.taskWindows(), m_applications, m_settings)
    , m_trash(trash ? std::move(trash) : std::make_unique<TrashMonitor>())
{
    m_subscriptions.push_back(m_settings.directionChanged.connect([this] {
        if (m_object)
            m_object->emitSignal("directionChanged");
    }));
    m_subscriptions.push_back(m_settings.visibilityChanged.connect([this] {
        if (m_object)
            m_object->emitSignal("visibilityChanged");
    }));
}

Dock::~Dock()
{
    if (m_object && m_bus)
        m_bus->releaseName("com.cutefish.Dock");
    waitForFileThread();
}

void Dock::notifyPrimaryGeometryChanged()
{
    primaryGeometryChanged();
    if (m_object)
        m_object->emitSignal("primaryGeometryChanged");
}

bool Dock::registerService()
{
    if (m_object)
        return true;
    if (!m_bus)
        return false;

    auto object = std::make_unique<dbus::Object>(m_bus, "/Dock", "com.cutefish.Dock");
    object->addMethod("add", [this](const std::string &desktopFile) { m_applications.addItem(desktopFile); },
                      dbus::ArgumentNames{{"desktopFile"}, {}});
    object->addMethod("remove", [this](const std::string &desktopFile) { m_applications.removeItem(desktopFile); },
                      dbus::ArgumentNames{{"desktopFile"}, {}});
    object->addMethod("pinned",
                      [this](const std::string &desktopFile) { return m_applications.isDesktopPinned(desktopFile); },
                      dbus::ArgumentNames{{"desktopFile"}, {"result"}});
    object->addMethod("setDirection",
                      [this](int32_t direction) { m_settings.setDirection(DockSettings::Direction(direction)); },
                      dbus::ArgumentNames{{"direction"}, {}});
    object->addMethod("setIconSize", [this](int32_t iconSize) { m_settings.setIconSize(iconSize); },
                      dbus::ArgumentNames{{"iconSize"}, {}});
    object->addMethod("setVisibility",
                      [this](int32_t visibility) { m_settings.setVisibility(DockSettings::Visibility(visibility)); },
                      dbus::ArgumentNames{{"visibility"}, {}});
    object->addMethod("setGlassTint", [this](double tint) { m_settings.setGlassTint(tint); },
                      dbus::ArgumentNames{{"tint"}, {}});
    object->addMethod("setMinimizedArea", [this](bool enabled) { m_settings.setMinimizedArea(enabled); },
                      dbus::ArgumentNames{{"enabled"}, {}});
    object->addProperty("primaryGeometry", [this] {
        const base::Rect rect = primaryGeometry ? primaryGeometry() : base::Rect();
        return std::tuple<int32_t, int32_t, int32_t, int32_t>(rect.x(), rect.y(), rect.width(), rect.height());
    });
    object->addProperty("direction", [this] { return int32_t(m_settings.direction()); });
    object->addProperty("visibility", [this] { return int32_t(m_settings.visibility()); });
    object->addSignal("primaryGeometryChanged", "");
    object->addSignal("directionChanged", "");
    object->addSignal("visibilityChanged", "");
    if (!object->publish() || !m_bus->requestName("com.cutefish.Dock", dbus::Connection::DoNotQueue)) {
        CF_WARNING(cfDock) << "Cannot own com.cutefish.Dock";
        return false;
    }
    m_object = std::move(object);
    return true;
}

} // namespace cutefish::shell
