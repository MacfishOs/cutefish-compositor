// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_COMPUTER_CONTROL_COMPUTER_CONTROL_SERVICE_H_
#define CUTEFISH_COMPUTER_CONTROL_COMPUTER_CONTROL_SERVICE_H_

#include <memory>

#include "base/dbus.h"
#include "base/signal.h"

namespace cutefish::computer_control {

class Controller;

// com.cutefish.ComputerControl1: whether an agent is in control, for the
// status bar's Stop button. Stopping is never a privilege, so any session
// client may call Stop.
class ComputerControlService
{
public:
    // The session bus when no connection is given.
    explicit ComputerControlService(Controller *controller,
                                    base::dbus::Connection *bus = nullptr);
    ComputerControlService(const ComputerControlService &) = delete;
    ComputerControlService &operator=(const ComputerControlService &) = delete;
    ~ComputerControlService();

    bool registerService();

private:
    Controller *m_controller;
    base::dbus::Connection *m_bus;
    std::unique_ptr<base::dbus::Object> m_object;
    base::Subscription m_activeChanged;
    bool m_registered = false;
};

} // namespace cutefish::computer_control

#endif // CUTEFISH_COMPUTER_CONTROL_COMPUTER_CONTROL_SERVICE_H_
