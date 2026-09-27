// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_GREETD_BACKEND_H
#define CUTEFISH_AUTH_GREETD_BACKEND_H
#include "auth/auth_backend.h"
#include <QJsonObject>
#include <QTimer>
class QLocalSocket;

namespace cutefish::auth {
class GreetdBackend : public AuthBackend
{
    Q_OBJECT
public:
    explicit GreetdBackend(QObject *parent = nullptr);
    ~GreetdBackend() override;

    // Whoever logged in last, as cutefish-dm records it.
    QString defaultUserName() const override { return lastLoginUser(); }

    void start() override;
    void authenticate(const QString &userName, Secret secret) override;
    void reset() override;

    void accept() override { }

    void setSession(const LoginSession &session) override { m_session = session; }

private:
    enum class Phase {
        Idle,
        Authenticating,
        Cancelling,
        Starting,
        Complete
    };
    void send(const QJsonObject &request);
    void readResponses();
    void handleResponse(const QJsonObject &response);
    void createSession();
    void fail();
    QLocalSocket *m_socket;
    QTimer m_timeout;
    QByteArray m_buffer;
    QString m_userName;
    Secret m_pending;
    LoginSession m_session;
    Phase m_phase = Phase::Idle;
    bool m_sessionOpen = false;
    bool m_retry = false;
};
} // namespace cutefish::auth
#endif
