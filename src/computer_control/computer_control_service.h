// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_COMPUTER_CONTROL_COMPUTER_CONTROL_SERVICE_H_
#define CUTEFISH_COMPUTER_CONTROL_COMPUTER_CONTROL_SERVICE_H_

#include <QDBusConnection>
#include <QObject>

namespace cutefish::computer_control {

class Controller;

// com.cutefish.ComputerControl1: whether an agent is in control, for the
// status bar's Stop button. Stopping is never a privilege, so any session
// client may call Stop.
class ComputerControlService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.ComputerControl1")

public:
    explicit ComputerControlService(Controller *controller,
                                    QDBusConnection bus = QDBusConnection::sessionBus());
    ~ComputerControlService() override;

    bool registerService();

public slots:
    Q_SCRIPTABLE bool GetActive() const;
    // The agent's own name for itself; empty while no agent is in control.
    Q_SCRIPTABLE QString GetAgentName() const;
    Q_SCRIPTABLE void Stop();

signals:
    Q_SCRIPTABLE void ActiveChanged(bool active);

private:
    Controller *m_controller;
    QDBusConnection m_bus;
    bool m_registered = false;
};

} // namespace cutefish::computer_control

#endif // CUTEFISH_COMPUTER_CONTROL_COMPUTER_CONTROL_SERVICE_H_
