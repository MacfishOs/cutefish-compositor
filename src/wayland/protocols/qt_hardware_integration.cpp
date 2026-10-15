// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/qt_hardware_integration.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <hardware-integration-server-protocol.h>
#include <optional>
#include <sys/stat.h>
#include <sys/types.h>

#include "base/file.h"

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

std::optional<FileId> fileId(const std::string& path)
{
    struct stat info {};
    if (stat(path.c_str(), &info) != 0)
        return std::nullopt;
    return FileId {info.st_dev, info.st_ino};
}

// The host's Qt 6 library `name`, as the file its soname link points to. Looked
// up on every check: a Qt upgrade replaces it.
std::string hostLibrary(std::string_view name)
{
    const std::string link =
            std::string(CUTEFISH_QT_LIBRARY_DIR) + '/' + std::string(name) + ".so.6";
    char resolved[PATH_MAX];
    return realpath(link.c_str(), resolved) ? std::string(resolved) : std::string();
}

// "6.8.2" from ".../libQt6Gui.so.6.8.2".
std::string libraryVersion(const std::string& path, std::string_view name)
{
    const std::string prefix = std::string(name) + ".so.";
    const size_t slash = path.rfind('/');
    const std::string_view fileName =
            std::string_view(path).substr(slash == std::string::npos ? 0 : slash + 1);
    return fileName.starts_with(prefix) ? std::string(fileName.substr(prefix.size()))
                                        : std::string();
}

// Just enough CBOR (RFC 8949) to walk Qt's plugin metadata.
class CborReader
{
public:
    CborReader(const uint8_t* data, size_t size) : m_data(data), m_end(data + size) { }

    bool atBreak() const { return m_data < m_end && *m_data == 0xff; }

    bool skipBreak()
    {
        if (!atBreak())
            return false;
        ++m_data;
        return true;
    }

    // Reads an item's head. `indefinite` is set for an indefinite-length
    // string, array or map.
    bool head(int& major, uint64_t& value, bool& indefinite)
    {
        if (m_data >= m_end)
            return false;
        const uint8_t initial = *m_data++;
        major = initial >> 5;
        const uint8_t info = initial & 0x1f;
        indefinite = false;
        if (info < 24) {
            value = info;
            return true;
        }
        if (info == 31) {
            indefinite = major >= 2 && major <= 5;
            value = 0;
            return indefinite;
        }
        if (info > 27)
            return false;
        const size_t bytes = size_t(1) << (info - 24);
        if (size_t(m_end - m_data) < bytes)
            return false;
        value = 0;
        for (size_t i = 0; i < bytes; ++i)
            value = (value << 8) | *m_data++;
        return true;
    }

    bool text(std::string& out)
    {
        int major = 0;
        uint64_t length = 0;
        bool indefinite = false;
        if (!head(major, length, indefinite) || major != 3 || indefinite)
            return false;
        if (uint64_t(m_end - m_data) < length)
            return false;
        out.assign(reinterpret_cast<const char*>(m_data), size_t(length));
        m_data += length;
        return true;
    }

    bool skip(int depth = 0)
    {
        if (depth > 16)
            return false;
        int major = 0;
        uint64_t value = 0;
        bool indefinite = false;
        if (!head(major, value, indefinite))
            return false;
        switch (major) {
        case 0:
        case 1:
        case 7:
            return true;
        case 2:
        case 3:
            if (indefinite) {
                while (!atBreak()) {
                    if (!skip(depth + 1))
                        return false;
                }
                return skipBreak();
            }
            if (uint64_t(m_end - m_data) < value)
                return false;
            m_data += value;
            return true;
        case 4:
        case 5: {
            const uint64_t items = major == 5 ? value * 2 : value;
            if (indefinite) {
                while (!atBreak()) {
                    if (!skip(depth + 1))
                        return false;
                }
                return skipBreak();
            }
            for (uint64_t i = 0; i < items; ++i) {
                if (!skip(depth + 1))
                    return false;
            }
            return true;
        }
        case 6:
            return skip(depth + 1);
        }
        return false;
    }

    // Calls `entry(key)` for every entry of the map at the cursor; the
    // callback must consume the value. Keys are integers or text.
    template<typename Entry>
    bool map(Entry entry)
    {
        int major = 0;
        uint64_t count = 0;
        bool indefinite = false;
        if (!head(major, count, indefinite) || major != 5)
            return false;
        for (uint64_t i = 0; indefinite ? !atBreak() : i < count; ++i) {
            const uint8_t* key = m_data;
            int keyMajor = 0;
            uint64_t keyValue = 0;
            bool keyIndefinite = false;
            if (!head(keyMajor, keyValue, keyIndefinite))
                return false;
            if (keyMajor == 0) {
                if (!entry(int64_t(keyValue), std::string()))
                    return false;
            } else if (keyMajor == 3) {
                m_data = key;
                std::string name;
                if (!text(name) || !entry(int64_t(-1), name))
                    return false;
            } else {
                m_data = key;
                if (!skip() || !skip())
                    return false;
            }
        }
        return !indefinite || skipBreak();
    }

private:
    const uint8_t* m_data;
    const uint8_t* m_end;
};

// The payload of the "qt-project!" note Qt 6 puts in a plugin's
// .note.qt.metadata section: a four-byte header, then CBOR.
std::string_view qtMetaDataNote(std::string_view file)
{
    if (file.size() < sizeof(Elf64_Ehdr) || std::memcmp(file.data(), ELFMAG, SELFMAG) != 0
        || file[EI_CLASS] != ELFCLASS64)
        return {};
    Elf64_Ehdr header;
    std::memcpy(&header, file.data(), sizeof(header));
    if (header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shoff > file.size()
        || (file.size() - header.e_shoff) / sizeof(Elf64_Shdr) < header.e_shnum)
        return {};
    for (uint16_t index = 0; index < header.e_shnum; ++index) {
        Elf64_Shdr section;
        std::memcpy(&section, file.data() + header.e_shoff + index * sizeof(Elf64_Shdr),
                    sizeof(section));
        if (section.sh_type != SHT_NOTE || section.sh_offset > file.size()
            || section.sh_size > file.size() - section.sh_offset)
            continue;
        std::string_view notes = file.substr(section.sh_offset, section.sh_size);
        while (notes.size() >= sizeof(Elf64_Nhdr)) {
            Elf64_Nhdr note;
            std::memcpy(&note, notes.data(), sizeof(note));
            const size_t nameSize = (size_t(note.n_namesz) + 3) & ~size_t(3);
            const size_t descSize = (size_t(note.n_descsz) + 3) & ~size_t(3);
            if (nameSize > notes.size() - sizeof(note)
                || descSize > notes.size() - sizeof(note) - nameSize)
                break;
            const std::string_view name = notes.substr(sizeof(note), note.n_namesz);
            constexpr std::string_view kOwner("qt-project!", 12);
            if (note.n_type == 0x74510001 && name == kOwner)
                return notes.substr(sizeof(note) + nameSize, note.n_descsz);
            notes.remove_prefix(sizeof(note) + nameSize + descSize);
        }
    }
    return {};
}

// The plugin records the Qt versions whose private ABI it was compiled against;
// they must be the host's. Read on every check, without loading the plugin:
// packages can change while the session runs.
bool pluginMatchesHostQt()
{
    const std::string path = std::string(CUTEFISH_QT_PLUGIN_DIR)
            + "/wayland-graphics-integration-client/libcutefish-egl.so";
    if (!fileId(path))
        return false;
    const std::optional<std::string> file = base::file::read(path);
    const QtPluginVersions versions = file ? qtPluginVersions(*file) : QtPluginVersions();
    const auto hostVersion = [](std::string_view name) {
        return libraryVersion(hostLibrary(name), name);
    };
    return !versions.gui.empty() && versions.gui == hostVersion("libQt6Gui")
            && !versions.waylandClient.empty()
            && versions.waylandClient == hostVersion("libQt6WaylandClient");
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

QtPluginVersions qtPluginVersions(std::string_view file)
{
    const std::string_view note = qtMetaDataNote(file);
    QtPluginVersions versions;
    if (note.size() <= 4)
        return versions;
    CborReader reader(reinterpret_cast<const uint8_t*>(note.data()) + 4, note.size() - 4);
    // Key 4 holds the plugin's own JSON metadata.
    reader.map([&](int64_t key, const std::string&) {
        if (key != 4)
            return reader.skip();
        return reader.map([&](int64_t, const std::string& name) {
            if (name == "QtGuiVersion")
                return reader.text(versions.gui);
            if (name == "QtWaylandClientVersion")
                return reader.text(versions.waylandClient);
            return reader.skip();
        });
    });
    return versions;
}

std::string findMappedQtWaylandClient(std::string_view maps)
{
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    };
    while (!maps.empty()) {
        const size_t newline = maps.find('\n');
        const std::string_view line = maps.substr(0, newline);
        maps.remove_prefix(newline == std::string_view::npos ? maps.size() : newline + 1);
        const size_t pathStart = line.find('/');
        if (pathStart == std::string_view::npos
            || line.find("/libQt6WaylandClient.so") == std::string_view::npos)
            continue;
        std::string_view path = line.substr(pathStart);
        while (!path.empty() && space(path.back()))
            path.remove_suffix(1);
        return std::string(path);
    }
    return std::string();
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
    const std::string process = "/proc/" + std::to_string(pid);
    const std::optional<std::string> maps = base::file::read(process + "/maps");
    if (!maps)
        return false;
    const std::string mapped = findMappedQtWaylandClient(*maps);

    // Resolved through the client's root: a sandbox's own copy at the same path,
    // or a file replaced by a Qt upgrade since the client started, is not the host's.
    const std::optional<FileId> host = fileId(hostLibrary("libQt6WaylandClient"));
    return host && !mapped.empty() && fileId(process + "/root" + mapped) == host;
}

} // namespace cutefish::wayland
