// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QObject>
#include <functional>
#include <vector>

namespace cutefish::wm {
class QuitAll;
}

namespace cutefish::session {

// com.cutefish.Applications1, which the session asks to quit every application
// before it logs out, restarts or shuts down.
class ApplicationsService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.Applications1")
public:
    // `setEnding` puts the docks and panels away while applications quit, and
    // brings them back if one keeps the session from ending.
    ApplicationsService(wm::QuitAll *quitAll, std::function<void(bool)> setEnding,
                        QDBusConnection bus = QDBusConnection::sessionBus());
    ~ApplicationsService() override;

    bool registerService();

public slots:
    // Replies once every application has quit, "quit", or once one has kept
    // the session from ending, "cancelled" or "unresponsive" with its id.
    // Concurrent callers share the answer.
    Q_SCRIPTABLE QString QuitAll(QString &applicationId);

private:
    wm::QuitAll *m_quitAll;
    std::function<void(bool)> m_setEnding;
    QDBusConnection m_bus;
    std::vector<QDBusMessage> m_pending;
    bool m_registered = false;
};

} // namespace cutefish::session
