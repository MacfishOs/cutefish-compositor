// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/auth_controller.h"
#include "auth/auth_backend.h"
#include "auth/secret.h"

#include <QFileInfo>
#include <QLoggingCategory>

#include <pwd.h>
#include <unistd.h>

// Off by default. Nothing logged from this category ever carries a credential:
// the password never reaches a QDebug stream, in any build.
Q_LOGGING_CATEGORY(cfAuth, "cutefish.auth", QtWarningMsg)

namespace cutefish::auth {

namespace {

QString accountDisplayName(const QString &userName)
{
    const QByteArray name = userName.toUtf8();
    if (const passwd *entry = getpwnam(name.constData())) {
        // GECOS is comma-separated; the first field is the full name.
        const QString gecos = QString::fromLocal8Bit(entry->pw_gecos);
        const QString full = gecos.section(QLatin1Char(','), 0, 0).trimmed();
        if (!full.isEmpty())
            return full;
    }
    return userName;
}

QString accountIcon(const QString &userName)
{
    const QByteArray name = userName.toUtf8();
    const passwd *entry = getpwnam(name.constData());
    const QString home = entry ? QString::fromLocal8Bit(entry->pw_dir) : QString();

    // AccountsService first, then the per-home face file: the same two places
    // the rest of Cutefish looks, and neither needs a running service.
    const QStringList candidates {
            QStringLiteral("/var/lib/AccountsService/icons/%1").arg(userName),
            home.isEmpty() ? QString() : home + QStringLiteral("/.face"),
            home.isEmpty() ? QString() : home + QStringLiteral("/.face.icon"),
    };
    for (const QString &path : candidates) {
        if (!path.isEmpty() && QFileInfo::exists(path))
            return path;
    }
    return QString();
}

} // namespace

AuthController::AuthController(Mode mode, AuthBackend *backend, QObject *parent)
    : QObject(parent)
    , m_mode(mode)
    , m_backend(backend)
{
    Q_ASSERT(m_backend);

    connect(m_backend, &AuthBackend::promptChanged, this, [this](const QString &prompt) {
        if (m_prompt == prompt)
            return;
        m_prompt = prompt;
        Q_EMIT promptChanged();
    });

    connect(m_backend, &AuthBackend::busyChanged, this, [this](bool busy) {
        if (busy && m_state != Authenticating)
            setState(Authenticating);
    });

    connect(m_backend, &AuthBackend::failed, this, [this](const QString &message) {
        if (m_state == Succeeded)
            return;
        setState(Failed, message);
        Q_EMIT authenticationFailed();
    });

    connect(m_backend, &AuthBackend::succeeded, this, [this] {
        // Only an outstanding attempt can authorize a transition.
        if (m_accepted || m_state != Authenticating)
            return;
        m_accepted = true;
        setState(Succeeded);

        // accept() before the signal, so a handler that tears the process down
        // cannot run before the request that unlocks the session was sent.
        m_backend->accept();
        Q_EMIT authenticationSucceeded();
    });

    resolveUser();
    if (m_mode == Login) {
        setUsers(findLoginUsers());
        setSessions(findLoginSessions(loginSessionDirectories()));
    }
}

QStringList AuthController::userNames() const
{
    QStringList names;
    for (const QString &user : m_users)
        names.append(accountDisplayName(user));
    return names;
}

void AuthController::setUsers(QStringList users)
{
    m_users = std::move(users);
    Q_EMIT usersChanged();
    if (m_users.isEmpty() || m_users.contains(m_userName)) {
        Q_EMIT userChanged();
        return;
    }
    // Whoever logged in last, else the first account: nobody types a name
    // the system already knows.
    const QString last = m_backend->defaultUserName();
    setUserName(m_users.contains(last) ? last : m_users.first());
}

void AuthController::setUserIndex(int index)
{
    if (index >= 0 && index < m_users.size())
        setUserName(m_users.at(index));
}

AuthController::~AuthController() = default;

QStringList AuthController::sessionNames() const
{
    QStringList names;
    for (const LoginSession &session : m_sessions)
        names.append(session.name);
    return names;
}

void AuthController::setSessions(QList<LoginSession> sessions)
{
    m_sessions = std::move(sessions);
    m_sessionIndex = -1;
    Q_EMIT sessionsChanged();
    selectSessionFor(m_userName);
}

void AuthController::setSessionIndex(int index)
{
    if (index < 0 || index >= m_sessions.size() || index == m_sessionIndex || busy() || succeeded())
        return;
    m_sessionIndex = index;
    Q_EMIT sessionChanged();
}

void AuthController::selectSessionFor(const QString &userName)
{
    if (m_sessions.isEmpty())
        return;
    // The user's last session, else Cutefish, else the first there is.
    const QString remembered = userName.isEmpty() ? QString() : lastLoginSession(userName);
    int index = -1;
    for (const QString &id : {remembered, QStringLiteral("cutefish-wayland")}) {
        for (int i = 0; index < 0 && !id.isEmpty() && i < m_sessions.size(); ++i) {
            if (m_sessions.at(i).id == id)
                index = i;
        }
    }
    index = qMax(index, 0);
    if (index == m_sessionIndex)
        return;
    m_sessionIndex = index;
    Q_EMIT sessionChanged();
}

void AuthController::resolveUser()
{
    m_userName = m_backend->defaultUserName();
    if (m_userName.isEmpty() && m_mode == Lock) {
        if (const passwd *entry = getpwuid(getuid()))
            m_userName = QString::fromLocal8Bit(entry->pw_name);
    }
    m_displayName = m_userName.isEmpty() ? QString() : accountDisplayName(m_userName);
    m_iconSource = m_userName.isEmpty() ? QString() : accountIcon(m_userName);
    Q_EMIT userChanged();
}

void AuthController::setUserName(const QString &name)
{
    if (!canSelectUser() || busy() || succeeded())
        return;
    m_userName = name.trimmed();
    m_displayName = accountDisplayName(m_userName);
    m_iconSource = accountIcon(m_userName);
    emit userChanged();
    selectSessionFor(m_userName);
}

void AuthController::start()
{
    m_backend->start();
}

void AuthController::authenticate(QString password)
{
    Secret secret(password);
    wipeString(password);

    if (m_state == Authenticating || m_state == Succeeded)
        return;

    setState(Authenticating);
    if (m_sessionIndex >= 0 && m_sessionIndex < m_sessions.size())
        m_backend->setSession(m_sessions.at(m_sessionIndex));
    m_backend->authenticate(m_userName, std::move(secret));
}

void AuthController::reset()
{
    // A success is not something the user can type their way out of.
    if (m_state == Succeeded || m_state == Authenticating)
        return;

    m_backend->reset();
    if (!m_prompt.isEmpty()) {
        m_prompt.clear();
        Q_EMIT promptChanged();
    }
    setState(Idle);
}

void AuthController::setState(State state, const QString &errorMessage)
{
    if (m_state == state && m_errorMessage == errorMessage)
        return;
    m_state = state;
    m_errorMessage = errorMessage;
    qCDebug(cfAuth) << "state" << state;
    Q_EMIT stateChanged();
}

} // namespace cutefish::auth
