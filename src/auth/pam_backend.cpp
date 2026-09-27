// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/pam_backend.h"
#include <pwd.h>
#include <unistd.h>

namespace cutefish::auth {
PamBackend::PamBackend(QObject *parent)
    : AuthBackend(parent)
{
    m_process.setProgram(QStringLiteral(CUTEFISH_AUTH_HELPER));
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.setStandardOutputFile(QProcess::nullDevice());
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        reset();
        emit failed(tr("Authentication timed out"));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this] {
        if (!m_pending)
            return;
        m_pending = false;
        m_timeout.stop();
        emit failed(tr("Authentication service unavailable"));
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                if (!m_pending)
                    return;
                m_pending = false;
                m_timeout.stop();
                if (status == QProcess::NormalExit && code == 0)
                    emit succeeded();
                else
                    emit failed(tr("Authentication failed"));
            });
}

PamBackend::~PamBackend()
{
    reset();
}

QString PamBackend::defaultUserName() const
{
    const passwd *user = getpwuid(getuid());
    return user ? QString::fromLocal8Bit(user->pw_name) : QString();
}

void PamBackend::authenticate(const QString &userName, Secret secret)
{
    if (m_pending || m_process.state() != QProcess::NotRunning)
        return;
    if (userName != defaultUserName() || secret.size() > 4096 || secret.isEmpty()) {
        emit failed(tr("Authentication failed"));
        return;
    }
    m_pending = true;
    m_process.start();
    if (!m_process.waitForStarted(1000))
        return;
    // The helper reads only this inherited pipe, never argv or a public socket.
    m_process.write(secret.constData(), secret.size());
    m_process.closeWriteChannel();
    secret.clear();
    m_timeout.start(30000);
}

void PamBackend::reset()
{
    m_pending = false;
    m_timeout.stop();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}
} // namespace cutefish::auth
