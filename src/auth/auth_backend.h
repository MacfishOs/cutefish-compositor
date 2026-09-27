// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_AUTH_BACKEND_H
#define CUTEFISH_AUTH_AUTH_BACKEND_H

#include "auth/login_sessions.h"
#include "auth/secret.h"

#include <QObject>
#include <QString>

namespace cutefish::auth {

class AuthBackend : public QObject
{
    Q_OBJECT

public:
    explicit AuthBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~AuthBackend() override = default;

    // Who the screen offers to authenticate as. Empty means the UI should ask.
    virtual QString defaultUserName() const = 0;

    // Open the conversation with the authentication service. Called once, after
    // the UI exists, so a prompt or an early error has somewhere to go.
    virtual void start() = 0;

    // Check one credential. The Secret is moved in and dies with the backend's
    // conversation; there is no copy left for the caller to worry about.
    virtual void authenticate(const QString &userName, Secret secret) = 0;

    // Drop any conversation in progress and return to the initial state. Must
    // leave no credential behind.
    virtual void reset() = 0;

    virtual void accept() = 0;

    // The session a login starts, chosen before authenticate(). Only a login
    // backend starts one; the others ignore it.
    virtual void setSession(const LoginSession &) { }

Q_SIGNALS:
    // The service asked something. Informational: the UI shows one password
    // field regardless, and an empty prompt means "the default".
    void promptChanged(const QString &prompt);

    // The credential was accepted. accept() may now be called, exactly once.
    void succeeded();

    // The credential was rejected, or the conversation failed. The UI clears
    // the field and shows `message`, which never contains the credential.
    void failed(const QString &message);

    // Whether a check is in flight. The UI disables input while it is.
    void busyChanged(bool busy);
};

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_AUTH_BACKEND_H
