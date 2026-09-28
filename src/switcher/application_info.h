// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SWITCHER_APPLICATION_INFO_H_
#define CUTEFISH_SWITCHER_APPLICATION_INFO_H_

#include <optional>

#include <QCache>
#include <QHash>
#include <QString>

namespace cutefish::switcher {

// What the dock shows for an application it cannot find either.
inline const QString kFallbackIcon = QStringLiteral("application-x-desktop");

// What the application switcher shows for an application.
struct ApplicationInfo {
    QString name;
    // An icon theme name, or an absolute path.
    QString icon;
};

// Finds an application's desktop entry from its window's application id, as
// the dock does: the entry named after the id, or one whose StartupWMClass is
// the id. Answers, found or not, are kept until an application directory
// changes.
class ApplicationInfoCache
{
public:
    // Forgets every answer once an application directory changed. Checked once
    // per list rather than once per entry: it reads each directory's time.
    void refresh();

    // `fallbackName` names an application with no desktop entry.
    ApplicationInfo lookup(const QString &applicationId, const QString &fallbackName);

private:
    // Lower-cased StartupWMClass to desktop file, read once per stamp.
    const QHash<QString, QString> &wmClasses();

    qint64 m_stamp = -1;
    // Bounded: a lookup that found nothing is cached like one that did, and
    // application ids come from clients.
    QCache<QString, ApplicationInfo> m_cache {256};
    std::optional<QHash<QString, QString>> m_wmClasses;
};

} // namespace cutefish::switcher

#endif // CUTEFISH_SWITCHER_APPLICATION_INFO_H_
