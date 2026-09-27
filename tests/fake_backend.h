// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_TESTS_FAKE_BACKEND_H
#define CUTEFISH_TESTS_FAKE_BACKEND_H

#include "auth/auth_backend.h"

#include <QStringList>

// A backend that answers whatever the test tells it to, and counts the things
// that must happen exactly once.
class FakeBackend : public cutefish::auth::AuthBackend
{

public:
    QString defaultUserName() const override { return QStringLiteral("tester"); }

    void start() override { ++startCount; }

    void authenticate(const QString &userName, cutefish::auth::Secret secret) override
    {
        lastUser = userName;
        // Recorded so a test can prove the credential arrived; a real backend
        // would never copy it anywhere that outlives the attempt.
        lastSecret = QString::fromUtf8(secret.constData(), int(secret.size()));
        ++authenticateCount;
    }

    void reset() override { ++resetCount; }

    void accept() override { ++acceptCount; }

    void setSession(const cutefish::auth::LoginSession &session) override { lastSession = session.id; }

    // Drive the controller the way a real service would.
    void succeed() { Q_EMIT succeeded(); }

    void reject(const QString &message = QStringLiteral("Incorrect password"))
    {
        Q_EMIT failed(message);
    }

    int startCount = 0;
    int authenticateCount = 0;
    int resetCount = 0;
    int acceptCount = 0;
    QString lastUser;
    QString lastSecret;
    QString lastSession;
};

#endif // CUTEFISH_TESTS_FAKE_BACKEND_H
