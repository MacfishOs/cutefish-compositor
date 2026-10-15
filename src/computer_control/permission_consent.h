// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_COMPUTER_CONTROL_PERMISSION_CONSENT_H_
#define CUTEFISH_COMPUTER_CONTROL_PERMISSION_CONSENT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "base/dbus.h"
#include "computer_control/controller.h"

namespace cutefish::computer_control {

// Asks the user through cutefish-services' permission prompt, kind
// "computer-control". Any session client can own a bus name first, so the
// owner's process has to be one that shows system dialogs, and the prompt goes
// to that owner's unique name.
class PermissionConsent
{
public:
    // The session bus when no connection is given.
    explicit PermissionConsent(std::function<bool(int64_t pid)> showsSystemDialogs,
                               base::dbus::Connection *bus = nullptr);
    PermissionConsent(const PermissionConsent &) = delete;
    PermissionConsent &operator=(const PermissionConsent &) = delete;
    ~PermissionConsent();

    // Environment::askConsent. Must outlive the questions it asks.
    std::function<void()> ask(const std::string &agentName,
                              std::function<void(ConsentResult)> answer);

private:
    struct Request;
    void prompt(const std::shared_ptr<Request> &request, const std::string &owner);

    std::function<bool(int64_t)> m_showsSystemDialogs;
    base::dbus::Connection *m_bus;
};

} // namespace cutefish::computer_control

#endif // CUTEFISH_COMPUTER_CONTROL_PERMISSION_CONSENT_H_
