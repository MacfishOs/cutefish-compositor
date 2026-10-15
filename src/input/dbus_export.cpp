// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/dbus_export.h"
#include "core/logging.h"


namespace cutefish::input {

std::unique_ptr<DBusExport> DBusExport::create(std::string service, std::string path,
                                               std::string interface)
{
    base::dbus::Connection *connection = base::dbus::Connection::session();
    if (!connection)
        return nullptr;
    return std::unique_ptr<DBusExport>(
            new DBusExport(connection, std::move(service), std::move(path), std::move(interface)));
}

DBusExport::DBusExport(base::dbus::Connection *connection, std::string service, std::string path,
                       std::string interface)
    : m_connection(connection)
    , m_service(std::move(service))
    , m_object(connection, std::move(path), std::move(interface))
{
}

DBusExport::~DBusExport()
{
    if (m_named)
        m_connection->releaseName(m_service);
}

bool DBusExport::publish(const char *what)
{
    if (!m_object.publish()) {
        CF_WARNING(cfInput).printf("Cannot export %s", what);
        return false;
    }
    if (!m_connection->requestName(m_service)) {
        CF_WARNING(cfInput).printf("Cannot register %s: %s is taken", what, m_service.c_str());
        return false;
    }
    m_named = true;
    return true;
}

void DBusExport::emitSignal(const char *member) const
{
    m_object.emitSignal(member);
}

} // namespace cutefish::input
