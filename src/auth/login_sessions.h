// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_LOGIN_SESSIONS_H
#define CUTEFISH_AUTH_LOGIN_SESSIONS_H

#include <QList>
#include <QString>
#include <QStringList>

namespace cutefish::auth {

// A Wayland session the login screen offers: a wayland-sessions entry, read as
// cutefish-dm reads it (cutefish-dm/daemon/sessions.cpp), so both agree on
// what exists.
struct LoginSession {
    QString id;
    QString name;
    QStringList command;
    // What pam_systemd and the session need for a daemon that, like greetd,
    // sets none of it; cutefish-dm sets its own from the id.
    QStringList environment;
};

QList<LoginSession> findLoginSessions(const QStringList &directories);

// /usr/local/share and /usr/share, as cutefish-dm looks. CUTEFISH_SESSION_DIRS,
// colon-separated, replaces them for tests.
QStringList loginSessionDirectories();

// The accounts a person logs in with, in UID order: UIDs within login.defs'
// UID_MIN..UID_MAX that have a login shell, as sddm's user list takes them.
QStringList findLoginUsers();

// cutefish-dm's record of the last login, from the file it keeps for the login
// screen to read. Empty under any other daemon.
QString lastLoginUser();
QString lastLoginSession(const QString &user);

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_LOGIN_SESSIONS_H
