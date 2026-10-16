// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/statusbar/status_bar.h"

#include "base/desktop_entry.h"
#include "base/logging.h"
#include "base/settings.h"
#include "powerprofiles/powerprofiles.h"
#include "shell/common/file_thread.h"
#include "shell/statusbar/clock_format.h"
#include "upower/batterystatus.h"

namespace cutefish::shell {

namespace dbus = base::dbus;

static const base::LogCategory cfStatusBar("cutefish.shell.statusbar");

StatusBar::StatusBar(dbus::Connection *sessionBus, dbus::Connection *systemBus, TaskManager *tasks,
                     std::string settingsPath)
    : m_bus(sessionBus)
    , m_settingsPath(std::move(settingsPath))
    , m_watcher(sessionBus)
    , m_tray(m_watcher, sessionBus, tasks)
    , m_registrar(sessionBus)
    , m_mpris(sessionBus)
    , m_battery(std::make_unique<Battery>(std::make_shared<upower::BatteryStatus>(sessionBus, systemBus),
                                          m_settingsPath))
    , m_brightness(sessionBus)
    , m_powerProfiles(std::make_unique<powerprofiles::PowerProfiles>(systemBus))
    , m_powerActions(sessionBus)
{
    m_battery->followService(sessionBus, [sessionBus, systemBus] {
        return std::make_shared<upower::BatteryStatus>(sessionBus, systemBus);
    });
    m_timeFormat = clock_format::pattern(m_timeLocale, m_twentyFourTime);

    m_load = runOnFileThread(
            [path = m_settingsPath] { return base::Settings(path).boolValue("Statusbar/TwentyFourTime", false); },
            [this](bool enabled) {
                // A choice made while the file was read stays.
                if (m_twentyFourTimeSet || enabled == m_twentyFourTime)
                    return;
                m_twentyFourTime = enabled;
                updateTimeFormat();
            });

    if (m_bus) {
        dbus::MatchRule rule;
        rule.sender = "com.cutefish.Services";
        rule.path = "/com/cutefish/Services/Language";
        rule.interface = "com.cutefish.Services.Language";
        rule.member = "languageChanged";
        m_languageChanged = m_bus->subscribe(rule, [this](const dbus::Message &) { reloadLanguage(); });
    }
    reloadLanguage();
}

StatusBar::~StatusBar()
{
    if (m_object && m_bus)
        m_bus->releaseName("com.cutefish.Statusbar");
}

bool StatusBar::registerServices()
{
    if (!m_object) {
        if (!m_bus)
            return false;
        auto object = std::make_unique<dbus::Object>(m_bus, "/Statusbar", "com.cutefish.Statusbar");
        object->addMethod("setBatteryPercentage", [this](bool enabled) { m_battery->setShowPercentage(enabled); },
                          dbus::ArgumentNames{{"batteryPercentage"}, {}});
        object->addMethod("setTwentyFourTime", [this](bool enabled) { setTwentyFourTime(enabled); },
                          dbus::ArgumentNames{{"t"}, {}});
        // The same as the menu's: the user is asked first. The session's
        // own methods act at once.
        object->addMethod("requestLogout", [this] { m_powerActions.request(PowerActions::Logout); });
        object->addMethod("requestReboot", [this] { m_powerActions.request(PowerActions::Reboot); });
        object->addMethod("requestShutdown", [this] { m_powerActions.request(PowerActions::Shutdown); });
        if (!object->publish() || !m_bus->requestName("com.cutefish.Statusbar", dbus::Connection::DoNotQueue)) {
            CF_WARNING(cfStatusBar) << "Cannot own com.cutefish.Statusbar";
            return false;
        }
        m_object = std::move(object);
    }
    const bool watcher = m_watcher.registerService();
    const bool registrar = m_registrar.registerService();
    return watcher && registrar;
}

void StatusBar::setTwentyFourTime(bool enabled)
{
    m_twentyFourTimeSet = true;
    if (m_twentyFourTime == enabled)
        return;
    m_twentyFourTime = enabled;
    writeSettings(m_settingsPath, [enabled](base::Settings &settings) {
        settings.setValue("Statusbar/TwentyFourTime", enabled);
    });
    updateTimeFormat();
}

void StatusBar::reloadLanguage()
{
    // The session language lives in cutefish-services' settings.
    m_language = runOnFileThread([] { return base::DesktopEntry::displayLanguage(); },
                                 [this](std::string language) {
                                     if (language == m_timeLocale)
                                         return;
                                     m_timeLocale = language;
                                     updateTimeFormat();
                                 });
}

void StatusBar::updateTimeFormat()
{
    m_timeFormat = clock_format::pattern(m_timeLocale, m_twentyFourTime);
    timeFormatChanged();
}

} // namespace cutefish::shell
