// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "computer_control/permission_consent.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUuid>
#include <QVariantMap>
#include <climits>
#include <memory>
#include <utility>

namespace cutefish::computer_control {

namespace {
const QString serviceName = QStringLiteral("com.cutefish.Services");
const QString permissionsPath = QStringLiteral("/com/cutefish/Services/Permissions");
const QString permissionsInterface = QStringLiteral("com.cutefish.Services.Permissions");

QDBusMessage busCall(const QString &method, const QString &argument)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), method);
    message << argument;
    return message;
}
} // namespace

struct PermissionConsent::Request {
    QString agentName;
    // Prompt tokens are shared by every caller of the service; a guessable
    // one could be taken first.
    QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Where the prompt went, for the dismissal.
    QString owner;
    std::function<void(ConsentResult)> answer;

    void finish(ConsentResult result)
    {
        if (auto callback = std::exchange(answer, {}))
            callback(result);
    }
};

PermissionConsent::PermissionConsent(std::function<bool(qint64 pid)> showsSystemDialogs,
                                     QDBusConnection bus, QObject *parent)
    : QObject(parent)
    , m_showsSystemDialogs(std::move(showsSystemDialogs))
    , m_bus(std::move(bus))
{
}

std::function<void()> PermissionConsent::ask(const QString &agentName,
                                             std::function<void(ConsentResult)> answer)
{
    auto request = std::make_shared<Request>();
    request->agentName = agentName;
    request->answer = std::move(answer);

    auto *owner = new QDBusPendingCallWatcher(
            m_bus.asyncCall(busCall(QStringLiteral("GetNameOwner"), serviceName)), this);
    connect(owner, &QDBusPendingCallWatcher::finished, this,
            [this, request](QDBusPendingCallWatcher *call) {
                call->deleteLater();
                const QDBusPendingReply<QString> reply = *call;
                if (!request->answer)
                    return;
                if (reply.isError()) {
                    request->finish(ConsentResult::Unavailable);
                    return;
                }
                prompt(request, reply.value());
            });

    return [this, request] {
        if (!std::exchange(request->answer, {}) || request->owner.isEmpty())
            return;
        QDBusMessage dismiss = QDBusMessage::createMethodCall(
                request->owner, permissionsPath, permissionsInterface, QStringLiteral("Dismiss"));
        dismiss << request->token;
        m_bus.send(dismiss);
    };
}

void PermissionConsent::prompt(const std::shared_ptr<Request> &request, const QString &owner)
{
    auto *pid = new QDBusPendingCallWatcher(
            m_bus.asyncCall(busCall(QStringLiteral("GetConnectionUnixProcessID"), owner)), this);
    connect(pid, &QDBusPendingCallWatcher::finished, this,
            [this, request, owner](QDBusPendingCallWatcher *call) {
                call->deleteLater();
                const QDBusPendingReply<uint> reply = *call;
                if (!request->answer)
                    return;
                if (reply.isError() || !m_showsSystemDialogs(reply.value())) {
                    request->finish(ConsentResult::Unavailable);
                    return;
                }

                // A unique name is never reused, so the process checked above is
                // the one that answers.
                request->owner = owner;
                QDBusMessage message = QDBusMessage::createMethodCall(
                        owner, permissionsPath, permissionsInterface, QStringLiteral("Prompt"));
                message << request->token << QStringLiteral("computer-control") << QString()
                        << QString()
                        << QVariantMap {{QStringLiteral("agent_name"), request->agentName}};
                // The user answers in their own time.
                auto *prompt = new QDBusPendingCallWatcher(m_bus.asyncCall(message, INT_MAX), this);
                connect(prompt, &QDBusPendingCallWatcher::finished, this,
                        [request](QDBusPendingCallWatcher *call) {
                            call->deleteLater();
                            const QDBusPendingReply<uint, QVariantMap> reply = *call;
                            if (reply.isError()) {
                                request->finish(ConsentResult::Unavailable);
                                return;
                            }
                            switch (reply.argumentAt<0>()) {
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
                        });
            });
}

} // namespace cutefish::computer_control
