// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/qt_hardware_integration.h"

#include <algorithm>
#include <hardware-integration-server-protocol.h>
#include <optional>
#include <sys/stat.h>
#include <sys/types.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QPluginLoader>

namespace cutefish::wayland {
namespace {

constexpr uint32_t kQtHardwareIntegrationVersion = 1;
constexpr char kClientBufferIntegration[] = "cutefish-egl";

struct FileId {
    dev_t device = 0;
    ino_t inode = 0;

    bool operator==(const FileId& other) const
    {
        return device == other.device && inode == other.inode;
    }
};

std::optional<FileId> fileId(const QString& path)
{
    struct stat info {};
    if (stat(QFile::encodeName(path).constData(), &info) != 0)
        return std::nullopt;
    return FileId {info.st_dev, info.st_ino};
}

// The host's Qt 6 library `name`, as the file its soname link points to. Looked
// up on every check: a Qt upgrade replaces it.
QString hostLibrary(QLatin1StringView name)
{
    const QString link = QLibraryInfo::path(QLibraryInfo::LibrariesPath) + QLatin1Char('/') + name
            + QLatin1StringView(".so.6");
    return QFileInfo(link).canonicalFilePath();
}

// "6.8.2" from ".../libQt6Gui.so.6.8.2".
QString libraryVersion(const QString& path, QLatin1StringView name)
{
    const QString prefix = name + QLatin1StringView(".so.");
    const QStringView fileName = QStringView(path).mid(path.lastIndexOf(QLatin1Char('/')) + 1);
    return fileName.startsWith(prefix) ? fileName.mid(prefix.size()).toString() : QString();
}

// The plugin records the Qt versions whose private ABI it was compiled against;
// they must be the host's. Read on every check, without loading the plugin:
// packages can change while the session runs.
bool pluginMatchesHostQt()
{
    const QString path = QLibraryInfo::path(QLibraryInfo::PluginsPath)
            + QStringLiteral("/wayland-graphics-integration-client/libcutefish-egl.so");
    if (!fileId(path))
        return false;
    const QJsonObject metaData =
            QPluginLoader(path).metaData().value(QStringLiteral("MetaData")).toObject();
    const QString gui = metaData.value(QStringLiteral("QtGuiVersion")).toString();
    const QString waylandClient =
            metaData.value(QStringLiteral("QtWaylandClientVersion")).toString();
    const auto hostVersion = [](QLatin1StringView name) {
        return libraryVersion(hostLibrary(name), name);
    };
    return !gui.isEmpty() && gui == hostVersion(QLatin1StringView("libQt6Gui"))
            && !waylandClient.isEmpty()
            && waylandClient == hostVersion(QLatin1StringView("libQt6WaylandClient"));
}

void bindQtHardwareIntegration(wl_client* client, void*, uint32_t version, uint32_t id)
{
    auto* resource = wl_resource_create(client, &qt_hardware_integration_interface,
                                         std::min(version, kQtHardwareIntegrationVersion), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, nullptr, nullptr, nullptr);
    qt_hardware_integration_send_client_backend(resource, kClientBufferIntegration);
}

} // namespace

wl_global* createQtHardwareIntegrationGlobal(wl_display* display)
{
    return wl_global_create(display, &qt_hardware_integration_interface,
                            kQtHardwareIntegrationVersion, nullptr, bindQtHardwareIntegration);
}

QString findMappedQtWaylandClient(QIODevice& maps)
{
    // /proc files report size 0, so QIODevice::atEnd() holds from the start.
    for (QByteArray line = maps.readLine(); !line.isEmpty(); line = maps.readLine()) {
        const qsizetype pathStart = line.indexOf('/');
        if (pathStart >= 0 && line.contains("/libQt6WaylandClient.so"))
            return QFile::decodeName(line.mid(pathStart).trimmed());
    }
    return QString();
}

bool canLoadCutefishEgl(const wl_client* client, bool sessionClient)
{
    if (!pluginMatchesHostQt())
        return false;

    // The session creates the shell's socketpair, so the peer credentials name
    // the session, not the shell. The shell is always built on the host's Qt.
    if (sessionClient)
        return true;

    pid_t pid = 0;
    wl_client_get_credentials(const_cast<wl_client*>(client), &pid, nullptr, nullptr);
    if (pid <= 0)
        return false;

    // The Qt platform plugin, and QtWaylandClient with it, is loaded before the
    // client connects, so it is mapped by the time the registry is filtered.
    QFile maps(QStringLiteral("/proc/%1/maps").arg(pid));
    if (!maps.open(QIODevice::ReadOnly))
        return false;
    const QString mapped = findMappedQtWaylandClient(maps);

    // Resolved through the client's root: a sandbox's own copy at the same path,
    // or a file replaced by a Qt upgrade since the client started, is not the host's.
    const std::optional<FileId> host =
            fileId(hostLibrary(QLatin1StringView("libQt6WaylandClient")));
    return host && !mapped.isEmpty()
            && fileId(QStringLiteral("/proc/%1/root").arg(pid) + mapped) == host;
}

} // namespace cutefish::wayland
