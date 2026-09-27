// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_AUTH_CONTROLLER_H
#define CUTEFISH_AUTH_AUTH_CONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>

#include "auth/login_sessions.h"

namespace cutefish::auth {

class AuthBackend;

class AuthController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int mode READ modeValue CONSTANT)
    Q_PROPERTY(QString userName READ userName WRITE setUserName NOTIFY userChanged)
    Q_PROPERTY(bool canSelectUser READ canSelectUser CONSTANT)
    Q_PROPERTY(QString displayName READ displayName NOTIFY userChanged)
    Q_PROPERTY(QString iconSource READ iconSource NOTIFY userChanged)
    Q_PROPERTY(QString prompt READ prompt NOTIFY promptChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(bool succeeded READ succeeded NOTIFY stateChanged)
    Q_PROPERTY(QStringList userNames READ userNames NOTIFY usersChanged)
    Q_PROPERTY(int userIndex READ userIndex WRITE setUserIndex NOTIFY userChanged)
    Q_PROPERTY(bool canPickUser READ canPickUser NOTIFY usersChanged)
    Q_PROPERTY(bool typesUserName READ typesUserName NOTIFY usersChanged)
    Q_PROPERTY(QStringList sessionNames READ sessionNames NOTIFY sessionsChanged)
    Q_PROPERTY(int sessionIndex READ sessionIndex WRITE setSessionIndex NOTIFY sessionChanged)
    Q_PROPERTY(bool canSelectSession READ canSelectSession NOTIFY sessionsChanged)

public:
    enum Mode {
        Login,
        Lock,
    };
    Q_ENUM(Mode)

    enum State {
        Idle,
        Authenticating,
        Failed,
        Succeeded,
    };
    Q_ENUM(State)

    AuthController(Mode mode, AuthBackend *backend, QObject *parent = nullptr);
    ~AuthController() override;

    Mode mode() const { return m_mode; }

    int modeValue() const { return int(m_mode); }

    State state() const { return m_state; }

    QString userName() const { return m_userName; }

    QString displayName() const { return m_displayName; }

    QString iconSource() const { return m_iconSource; }

    QString prompt() const { return m_prompt; }

    QString errorMessage() const { return m_errorMessage; }

    bool busy() const { return m_state == Authenticating; }

    bool failed() const { return m_state == Failed; }

    bool succeeded() const { return m_state == Succeeded; }

    bool canSelectUser() const { return m_mode == Login; }

    // Login only: the accounts to log in as, by display name, and which one.
    // With a single account there is nothing to pick; with none found the
    // name is typed instead.
    QStringList userNames() const;

    int userIndex() const { return int(m_users.indexOf(m_userName)); }

    void setUserIndex(int index);

    bool canPickUser() const { return m_users.size() > 1; }

    bool typesUserName() const { return m_mode == Login && m_users.isEmpty(); }

    void setUsers(QStringList users);

    // Login only: the wayland-sessions the login can start, and which one.
    // Choosing a user picks the session that user logged in with last.
    QStringList sessionNames() const;

    int sessionIndex() const { return m_sessionIndex; }

    void setSessionIndex(int index);

    bool canSelectSession() const { return m_sessions.size() > 1; }

    void setSessions(QList<LoginSession> sessions);

    void setUserName(const QString &name);
    void start();

    Q_INVOKABLE void authenticate(QString password);

    // Back to Idle: clears the error and the prompt. Called when the user
    // starts typing again after a rejection.
    Q_INVOKABLE void reset();

Q_SIGNALS:
    void stateChanged();
    void userChanged();
    void promptChanged();
    void sessionChanged();
    void sessionsChanged();
    void usersChanged();

    // The credential was rejected. The view shakes and clears its field; it
    // does not decide anything about the session.
    void authenticationFailed();

    // The backend confirmed, and has been told to proceed. Emitted once.
    void authenticationSucceeded();

private:
    void setState(State state, const QString &errorMessage = QString());
    void resolveUser();
    void selectSessionFor(const QString &userName);

    Mode m_mode;
    AuthBackend *m_backend = nullptr;
    State m_state = Idle;
    QString m_userName;
    QString m_displayName;
    QString m_iconSource;
    QString m_prompt;
    QString m_errorMessage;
    QStringList m_users;
    QList<LoginSession> m_sessions;
    int m_sessionIndex = -1;

    // accept() is irreversible - it unlocks the session or starts one - so it
    // is guarded rather than merely called from a single place.
    bool m_accepted = false;
};

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_AUTH_CONTROLLER_H
