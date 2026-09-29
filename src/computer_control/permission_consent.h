// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_COMPUTER_CONTROL_PERMISSION_CONSENT_H_
#define CUTEFISH_COMPUTER_CONTROL_PERMISSION_CONSENT_H_

#include <QDBusConnection>
#include <QObject>
#include <functional>

#include "computer_control/controller.h"

namespace cutefish::computer_control {

// Asks the user through cutefish-services' permission prompt, kind
// "computer-control". Any session client can own a bus name first, so the
// owner's process has to be one that shows system dialogs, and the prompt goes
// to that owner's unique name.
class PermissionConsent : public QObject
{
    Q_OBJECT

public:
    PermissionConsent(std::function<bool(qint64 pid)> showsSystemDialogs,
                      QDBusConnection bus = QDBusConnection::sessionBus(),
                      QObject *parent = nullptr);

    // Environment::askConsent. Must outlive the questions it asks.
    std::function<void()> ask(const QString &agentName, std::function<void(ConsentResult)> answer);

private:
    struct Request;

    void prompt(const std::shared_ptr<Request> &request, const QString &owner);

    std::function<bool(qint64)> m_showsSystemDialogs;
    QDBusConnection m_bus;
};

} // namespace cutefish::computer_control

#endif // CUTEFISH_COMPUTER_CONTROL_PERMISSION_CONSENT_H_
