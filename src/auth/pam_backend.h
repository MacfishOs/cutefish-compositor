// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_PAM_BACKEND_H
#define CUTEFISH_AUTH_PAM_BACKEND_H
#include "auth/auth_backend.h"
#include <QProcess>
#include <QTimer>

namespace cutefish::auth {
class PamBackend : public AuthBackend
{
    Q_OBJECT
public:
    explicit PamBackend(QObject *parent = nullptr);
    ~PamBackend() override;
    QString defaultUserName() const override;

    void start() override { }

    void authenticate(const QString &userName, Secret secret) override;
    void reset() override;

    void accept() override { }

private:
    QProcess m_process;
    QTimer m_timeout;
    bool m_pending = false;
};
} // namespace cutefish::auth
#endif
