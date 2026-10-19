// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "computer_control/permission_consent.h"
#include "core/main_loop.h"

#include "base/uuid.h"
#include <chrono>
#include <climits>
#include <utility>

namespace cutefish::computer_control {

namespace {

const std::string serviceName = "org.cutefish.Services";
const std::string permissionsPath = "/org/cutefish/Services/Permissions";
const std::string permissionsInterface = "org.cutefish.Services.Permissions";

} // namespace

struct PermissionConsent::Request {
    std::string agentName;
    // Prompt tokens are shared by every caller of the service; a guessable
    // one could be taken first.
    std::string token = base::Uuid::generate().toString();
    // Where the prompt went, for the dismissal.
    std::string owner;
    std::function<void(ConsentResult)> answer;
    // The calls in flight, dropped with the request.
    base::dbus::PendingCall ownerCall;
    base::dbus::PendingCall pidCall;
    base::dbus::PendingCall promptCall;

    void finish(ConsentResult result)
    {
        if (auto callback = std::exchange(answer, {}))
            callback(result);
    }
};

PermissionConsent::PermissionConsent(std::function<bool(int64_t pid)> showsSystemDialogs,
                                     base::dbus::Connection *bus)
    : m_showsSystemDialogs(std::move(showsSystemDialogs))
    , m_bus(bus ? bus : base::dbus::Connection::session(core::mainLoop()))
{
}

PermissionConsent::~PermissionConsent() = default;

std::function<void()> PermissionConsent::ask(const std::string &agentName,
                                             std::function<void(ConsentResult)> answer)
{
    auto request = std::make_shared<Request>();
    request->agentName = agentName;
    request->answer = std::move(answer);
    if (!m_bus) {
        request->finish(ConsentResult::Unavailable);
        return [] {};
    }
    const base::dbus::Proxy driver(m_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                   "org.freedesktop.DBus");
    // The request holds its own calls, so the callbacks hold it weakly.
    std::weak_ptr<Request> weak = request;
    request->ownerCall = driver.callAsync(
            "GetNameOwner",
            [this, weak](const base::dbus::Reply &reply) {
                const std::shared_ptr<Request> request = weak.lock();
                if (!request || !request->answer)
                    return;
                const std::optional<std::string> owner = reply.value<std::string>();
                if (!owner) {
                    request->finish(ConsentResult::Unavailable);
                    return;
                }
                prompt(request, *owner);
            },
            serviceName);
    return [this, request] {
        request->ownerCall.reset();
        request->pidCall.reset();
        if (!std::exchange(request->answer, {}) || request->owner.empty())
            return;
        request->promptCall.reset();
        base::dbus::Message dismiss = m_bus->newMethodCall(request->owner, permissionsPath,
                                                           permissionsInterface, "Dismiss");
        dismiss.append(request->token);
        m_bus->send(dismiss);
    };
}

void PermissionConsent::prompt(const std::shared_ptr<Request> &request, const std::string &owner)
{
    const base::dbus::Proxy driver(m_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                   "org.freedesktop.DBus");
    std::weak_ptr<Request> weak = request;
    request->pidCall = driver.callAsync(
            "GetConnectionUnixProcessID",
            [this, weak, owner](const base::dbus::Reply &reply) {
                const std::shared_ptr<Request> request = weak.lock();
                if (!request || !request->answer)
                    return;
                const std::optional<uint32_t> pid = reply.value<uint32_t>();
                if (!pid || !m_showsSystemDialogs(*pid)) {
                    request->finish(ConsentResult::Unavailable);
                    return;
                }
                // A unique name is never reused, so the process checked above is
                // the one that answers.
                request->owner = owner;
                base::dbus::Proxy permissions(m_bus, owner, permissionsPath, permissionsInterface);
                // The user answers in their own time.
                permissions.setTimeout(std::chrono::milliseconds(INT_MAX));
                request->promptCall = permissions.callAsync(
                        "Prompt",
                        [weak](const base::dbus::Reply &reply) {
                            const std::shared_ptr<Request> request = weak.lock();
                            if (!request)
                                return;
                            switch (reply.value<uint32_t>(0).value_or(2)) {
                            case 0:
                                request->finish(ConsentResult::Allowed);
                                break;
                            case 1:
                                request->finish(ConsentResult::Denied);
                                break;
                            default:
                                request->finish(ConsentResult::Unavailable);
                                break;
                            }
                        },
                        request->token, std::string("computer-control"), std::string(),
                        std::string(),
                        base::dbus::VariantMap {
                                {"agent_name", base::dbus::Variant(request->agentName)}});
            },
            owner);
}

} // namespace cutefish::computer_control
