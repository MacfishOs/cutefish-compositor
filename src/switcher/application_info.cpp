// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "switcher/application_info.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStandardPaths>

namespace cutefish::switcher {

namespace {

struct DesktopEntry {
    QString name;
    QString icon;
    QString startupWmClass;
    bool valid = false;
};

// The [Desktop Entry] group's Name in the user's language, Icon and
// StartupWMClass.
DesktopEntry readDesktopEntry(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString locale = QLocale::system().name();
    const QString language = locale.section(QLatin1Char('_'), 0, 0);
    DesktopEntry entry;
    QString localized;
    QString languageOnly;
    bool inGroup = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            if (inGroup)
                break;
            inGroup = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (!inGroup || separator <= 0)
            continue;
        const QString key = line.left(separator).trimmed();
        const QString value = line.mid(separator + 1).trimmed();
        if (key == QLatin1String("Name"))
            entry.name = value;
        else if (key == QStringLiteral("Name[%1]").arg(locale))
            localized = value;
        else if (key == QStringLiteral("Name[%1]").arg(language))
            languageOnly = value;
        else if (key == QLatin1String("Icon"))
            entry.icon = value;
        else if (key == QLatin1String("StartupWMClass"))
            entry.startupWmClass = value;
    }
    if (!localized.isEmpty())
        entry.name = localized;
    else if (!languageOnly.isEmpty())
        entry.name = languageOnly;
    entry.valid = !entry.name.isEmpty();
    return entry;
}

// Installing or removing an application changes its directory.
qint64 applicationsStamp()
{
    qint64 stamp = 0;
    for (const QString &directory :
         QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation)) {
        stamp = std::max(stamp, QFileInfo(directory).lastModified().toMSecsSinceEpoch());
    }
    return stamp;
}

} // namespace

void ApplicationInfoCache::refresh()
{
    const qint64 stamp = applicationsStamp();
    if (stamp == m_stamp)
        return;
    m_stamp = stamp;
    m_cache.clear();
    m_wmClasses.reset();
}

ApplicationInfo ApplicationInfoCache::lookup(const QString &applicationId,
                                             const QString &fallbackName)
{
    if (applicationId.isEmpty())
        return {fallbackName, kFallbackIcon};
    if (m_stamp < 0)
        refresh();
    if (const ApplicationInfo *cached = m_cache.object(applicationId))
        return *cached;

    DesktopEntry entry;
    for (const QString &candidate : {applicationId, applicationId.toLower()}) {
        const QString path = QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
                                                    candidate + QLatin1String(".desktop"));
        if (!path.isEmpty() && (entry = readDesktopEntry(path)).valid)
            break;
    }
    // An X11 class, or a Wayland id that is not the entry's file name.
    if (!entry.valid) {
        const QString path = wmClasses().value(applicationId.toLower());
        if (!path.isEmpty())
            entry = readDesktopEntry(path);
    }
    ApplicationInfo info;
    info.name = entry.valid ? entry.name : applicationId;
    info.icon = entry.valid && !entry.icon.isEmpty() ? entry.icon : kFallbackIcon;
    m_cache.insert(applicationId, new ApplicationInfo(info));
    return info;
}

const QHash<QString, QString> &ApplicationInfoCache::wmClasses()
{
    if (m_wmClasses)
        return *m_wmClasses;
    m_wmClasses.emplace();
    // Reversed, so an entry in an earlier directory shadows a later one.
    QStringList directories =
            QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    std::reverse(directories.begin(), directories.end());
    for (const QString &directory : directories) {
        const QDir dir(directory);
        for (const QString &file : dir.entryList({QStringLiteral("*.desktop")}, QDir::Files)) {
            const DesktopEntry entry = readDesktopEntry(dir.filePath(file));
            if (entry.valid && !entry.startupWmClass.isEmpty())
                m_wmClasses->insert(entry.startupWmClass.toLower(), dir.filePath(file));
        }
    }
    return *m_wmClasses;
}

} // namespace cutefish::switcher
