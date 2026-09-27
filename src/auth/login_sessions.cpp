// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/login_sessions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <pwd.h>
#include <utility>
#include <vector>

namespace cutefish::auth {

namespace {

const QString kStatePath = QStringLiteral("/var/lib/cutefish-dm/state.conf");

QString unescape(const QString &value)
{
    QString result;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c != QLatin1Char('\\') || i + 1 == value.size()) {
            result.append(c);
            continue;
        }
        const QChar next = value.at(++i);
        result.append(next == QLatin1Char('s')   ? QChar(QLatin1Char(' '))
                      : next == QLatin1Char('n') ? QChar(QLatin1Char('\n'))
                      : next == QLatin1Char('t') ? QChar(QLatin1Char('\t'))
                                                 : next);
    }
    return result;
}

bool executableExists(const QString &program)
{
    if (QFileInfo(program).isAbsolute())
        return QFileInfo(program).isExecutable();
    return !QStandardPaths::findExecutable(program).isEmpty();
}

bool readEntry(const QString &path, LoginSession *session)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QHash<QString, QString> keys;
    bool inEntry = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (inEntry && equals > 0 && !keys.contains(line.left(equals).trimmed()))
            keys.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
    }
    if (keys.value(QStringLiteral("Hidden")) == QLatin1String("true")
        || keys.value(QStringLiteral("NoDisplay")) == QLatin1String("true"))
        return false;
    const QString tryExec = unescape(keys.value(QStringLiteral("TryExec")));
    if (!tryExec.isEmpty() && !executableExists(tryExec))
        return false;

    session->id = QFileInfo(path).completeBaseName();
    session->name = unescape(keys.value(QStringLiteral("Name"), session->id));
    session->command.clear();
    for (const QString &argument :
         QProcess::splitCommand(unescape(keys.value(QStringLiteral("Exec"))))) {
        if (!(argument.size() == 2 && argument.startsWith(QLatin1Char('%'))))
            session->command.append(argument);
    }
    QStringList desktopNames;
    for (const QString &name : keys.value(QStringLiteral("DesktopNames")).split(QLatin1Char(';'))) {
        if (!name.trimmed().isEmpty())
            desktopNames.append(name.trimmed());
    }
    session->environment = {QStringLiteral("XDG_SESSION_TYPE=wayland"),
                            QStringLiteral("DESKTOP_SESSION=%1").arg(session->id),
                            QStringLiteral("XDG_SESSION_DESKTOP=%1")
                                    .arg(desktopNames.isEmpty() ? session->id : desktopNames.first())};
    if (!desktopNames.isEmpty())
        session->environment.append(
                QStringLiteral("XDG_CURRENT_DESKTOP=%1").arg(desktopNames.join(QLatin1Char(':'))));
    return !session->command.isEmpty();
}

} // namespace

QList<LoginSession> findLoginSessions(const QStringList &directories)
{
    QList<LoginSession> sessions;
    QSet<QString> seen;
    for (const QString &directory : directories) {
        const auto files = QDir(directory).entryInfoList({QStringLiteral("*.desktop")}, QDir::Files,
                                                         QDir::Name);
        for (const QFileInfo &file : files) {
            if (seen.contains(file.completeBaseName()))
                continue;
            seen.insert(file.completeBaseName());
            LoginSession session;
            if (readEntry(file.absoluteFilePath(), &session))
                sessions.append(session);
        }
    }
    return sessions;
}

QStringList loginSessionDirectories()
{
    const QString overridden = qEnvironmentVariable("CUTEFISH_SESSION_DIRS");
    if (!overridden.isEmpty())
        return overridden.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return {QStringLiteral("/usr/local/share/wayland-sessions"),
            QStringLiteral("/usr/share/wayland-sessions")};
}

QStringList findLoginUsers()
{
    uid_t minimum = 1000, maximum = 60000;
    QFile defs(QStringLiteral("/etc/login.defs"));
    if (defs.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!defs.atEnd()) {
            const QStringList fields =
                    QString::fromUtf8(defs.readLine()).simplified().split(QLatin1Char(' '));
            if (fields.size() == 2 && fields.first() == QLatin1String("UID_MIN"))
                minimum = fields.last().toUInt();
            else if (fields.size() == 2 && fields.first() == QLatin1String("UID_MAX"))
                maximum = fields.last().toUInt();
        }
    }
    std::vector<std::pair<uid_t, QString>> accounts;
    setpwent();
    while (const passwd *entry = getpwent()) {
        const QString shell = QString::fromLocal8Bit(entry->pw_shell);
        if (entry->pw_uid < minimum || entry->pw_uid > maximum
            || shell.endsWith(QLatin1String("/nologin")) || shell.endsWith(QLatin1String("/false")))
            continue;
        accounts.emplace_back(entry->pw_uid, QString::fromLocal8Bit(entry->pw_name));
    }
    endpwent();
    std::sort(accounts.begin(), accounts.end());
    QStringList users;
    for (const auto &account : accounts)
        users.append(account.second);
    return users;
}

QString lastLoginUser()
{
    return QSettings(kStatePath, QSettings::IniFormat).value(QStringLiteral("Last/User")).toString();
}

QString lastLoginSession(const QString &user)
{
    QSettings settings(kStatePath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Sessions"));
    return settings.value(user).toString();
}

} // namespace cutefish::auth
