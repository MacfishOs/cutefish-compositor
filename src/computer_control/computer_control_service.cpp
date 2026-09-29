// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "computer_control/computer_control_service.h"

#include <QDBusError>
#include <QDebug>
#include <utility>

#include "computer_control/controller.h"

namespace cutefish::computer_control {

namespace {
const QString serviceName = QStringLiteral("com.cutefish.ComputerControl1");
const QString objectPath = QStringLiteral("/com/cutefish/ComputerControl1");
} // namespace

ComputerControlService::ComputerControlService(Controller *controller, QDBusConnection bus)
    : m_controller(controller)
    , m_bus(std::move(bus))
{
    connect(m_controller, &Controller::activeChanged, this,
            &ComputerControlService::ActiveChanged);
}

ComputerControlService::~ComputerControlService()
{
    if (m_registered) {
        m_bus.unregisterService(serviceName);
        m_bus.unregisterObject(objectPath);
    }
}

bool ComputerControlService::registerService()
{
    if (m_registered)
        return true;
    if (!m_bus.registerObject(objectPath, this,
                              QDBusConnection::ExportScriptableSlots
                                      | QDBusConnection::ExportScriptableSignals)) {
        qWarning() << "Cannot export the computer control service:"
                   << m_bus.lastError().message();
        return false;
    }
    if (!m_bus.registerService(serviceName)) {
        qWarning() << "Cannot own the computer control service:" << m_bus.lastError().message();
        m_bus.unregisterObject(objectPath);
        return false;
    }
    m_registered = true;
    return true;
}

bool ComputerControlService::GetActive() const
{
    return m_controller->isActive();
}

QString ComputerControlService::GetAgentName() const
{
    return m_controller->agentName();
}

void ComputerControlService::Stop()
{
    m_controller->stop();
}

} // namespace cutefish::computer_control
