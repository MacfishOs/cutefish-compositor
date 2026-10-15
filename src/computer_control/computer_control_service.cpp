// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "computer_control/computer_control_service.h"
#include "core/main_loop.h"

#include "base/logging.h"
#include <utility>

#include "computer_control/controller.h"

namespace cutefish::computer_control {

namespace {

const base::LogCategory &logs()
{
    return base::LogCategory::defaultCategory();
}

const std::string serviceName = "com.cutefish.ComputerControl1";
const std::string objectPath = "/com/cutefish/ComputerControl1";
} // namespace

ComputerControlService::ComputerControlService(Controller *controller, base::dbus::Connection *bus)
    : m_controller(controller)
    , m_bus(bus ? bus : base::dbus::Connection::session(core::mainLoop()))
{
    m_activeChanged = m_controller->activeChanged.connect([this](bool active) {
        if (m_object)
            m_object->emitSignal("ActiveChanged", active);
    });
}

ComputerControlService::~ComputerControlService()
{
    if (m_registered)
        m_bus->releaseName(serviceName);
}

bool ComputerControlService::registerService()
{
    if (m_registered)
        return true;
    if (!m_bus) {
        CF_WARNING(logs()) << "Cannot export the computer control service: no session bus";
        return false;
    }
    auto object = std::make_unique<base::dbus::Object>(m_bus, objectPath, serviceName);
    object->addMethod("GetActive", [this] { return m_controller->isActive(); });
    // The agent's own name for itself; empty while no agent is in control.
    object->addMethod("GetAgentName", [this] { return m_controller->agentName(); });
    object->addMethod("Stop", [this] { m_controller->stop(); });
    object->addSignal("ActiveChanged", "b", {"active"});
    if (!object->publish()) {
        CF_WARNING(logs()) << "Cannot export the computer control service";
        return false;
    }
    if (!m_bus->requestName(serviceName)) {
        CF_WARNING(logs()) << "Cannot own the computer control service";
        return false;
    }
    m_object = std::move(object);
    m_registered = true;
    return true;
}

} // namespace cutefish::computer_control
