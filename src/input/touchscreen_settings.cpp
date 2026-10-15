// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/touchscreen_settings.h"
#include "core/logging.h"


#include "core/preferences.h"
#include "input/dbus_export.h"

namespace cutefish::input {

static constexpr auto service = "com.cutefish.Touchscreen1";
static constexpr auto path = "/com/cutefish/Touchscreen1";
static constexpr auto disabledDevicesKey = "Touchscreen/disabledDevices";

TouchscreenSettings::TouchscreenSettings(const std::string &settingsFile)
    : m_settings(settingsFile.empty() ? preferencesFile("compositor") : settingsFile)
{
    for (const std::string &name : m_settings.stringListValue(disabledDevicesKey)) {
        if (!name.empty() && deviceEnabled(name))
            m_disabledDevices.push_back(name);
    }
}

TouchscreenSettings::~TouchscreenSettings() = default;

bool TouchscreenSettings::registerService()
{
    if (m_object)
        return true;
    auto exported = DBusExport::create(service, path, service);
    if (!exported) {
        CF_WARNING(cfInput) << "Cannot export touchscreen settings: no session bus";
        return false;
    }
    base::dbus::Object &object = exported->object();
    object.addProperty("devices", [this] { return m_devices; });
    object.addProperty("disabledDevices", [this] { return m_disabledDevices; });
    object.addMethod("setDeviceEnabled", [this](const std::string &name, bool enabled) {
        setDeviceEnabled(name, enabled);
    }, base::dbus::ArgumentNames{{"name", "enabled"}, {}});
    for (const char *member : {"devicesChanged", "disabledDevicesChanged", "settingsChanged"})
        object.addSignal(member, "");
    if (!exported->publish("touchscreen settings"))
        return false;
    m_object = std::move(exported);
    return true;
}

void TouchscreenSettings::notify(const base::Signal<> &signal, const char *member)
{
    signal();
    if (m_object)
        m_object->emitSignal(member);
}

void TouchscreenSettings::setDevices(const std::vector<std::string> &devices)
{
    if (m_devices == devices)
        return;
    m_devices = devices;
    notify(devicesChanged, "devicesChanged");
}

void TouchscreenSettings::setDeviceEnabled(const std::string &name, bool enabled)
{
    if (name.empty() || deviceEnabled(name) == enabled)
        return;
    if (enabled)
        std::erase(m_disabledDevices, name);
    else
        m_disabledDevices.push_back(name);
    m_settings.setValue(disabledDevicesKey, m_disabledDevices);
    if (!m_settings.sync())
        CF_WARNING(cfInput) << "Cannot persist touchscreen settings to" << m_settings.path();
    notify(disabledDevicesChanged, "disabledDevicesChanged");
    notify(settingsChanged, "settingsChanged");
}
} // namespace cutefish::input
