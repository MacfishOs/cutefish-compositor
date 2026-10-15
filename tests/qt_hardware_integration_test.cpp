// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/qt_hardware_integration.h"

#include <cstring>
#include <elf.h>
#include <gtest/gtest.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace cutefish::wayland {
namespace {

std::string parse(const std::string& maps)
{
    return findMappedQtWaylandClient(maps);
}

// A 64-bit ELF file whose only note section holds `notes`.
std::string elfWithNotes(const std::string& notes)
{
    Elf64_Ehdr header {};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = 2;
    header.e_shoff = sizeof(header) + notes.size();
    Elf64_Shdr null {};
    Elf64_Shdr section {};
    section.sh_type = SHT_NOTE;
    section.sh_offset = sizeof(header);
    section.sh_size = notes.size();
    std::string file(reinterpret_cast<const char*>(&header), sizeof(header));
    file += notes;
    file.append(reinterpret_cast<const char*>(&null), sizeof(null));
    file.append(reinterpret_cast<const char*>(&section), sizeof(section));
    return file;
}

std::string note(const std::string& owner, uint32_t type, const std::string& description)
{
    const auto padded = [](std::string bytes) {
        bytes.resize((bytes.size() + 3) & ~size_t(3), '\0');
        return bytes;
    };
    Elf64_Nhdr header {uint32_t(owner.size() + 1), uint32_t(description.size()), type};
    return std::string(reinterpret_cast<const char*>(&header), sizeof(header))
            + padded(owner + '\0') + padded(description);
}

// The metadata note of a cutefish-egl plugin built against Qt 6.8.2.
const std::string kMetaData(
        "\x01\x06\x08\x01\xbf\x02\x78\x53org.qt-project.Qt.WaylandClient."
        "QWaylandClientBufferIntegrationFactoryInterface.5.3\x03\x78\x1d"
        "QWaylandEglClientBufferPlugin\x04\xa3\x64Keys\x81\x6c"
        "cutefish-egl\x6cQtGuiVersion\x65"
        "6.8.2\x76QtWaylandClientVersion\x65"
        "6.8.2\xff",
        0xc1);

TEST(QtHardwareIntegrationTest, FindsQtWaylandClient)
{
    const std::string path = parse(
            "7f10a000-7f10b000 r--p 00000000 fe:03 1311 "
            "/usr/lib/x86_64-linux-gnu/libQt6Core.so.6.8.2\n"
            "7f30a000-7f30b000 r--p 00000000 00:00 0 [heap]\n"
            "7f40a000-7f40b000 r-xp 00001000 fe:03 1313 "
            "/usr/lib/x86_64-linux-gnu/libQt6WaylandClient.so.6.8.2\n"
            "7f50a000-7f50b000 r-xp 00001000 fe:03 1314 "
            "/usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqwayland-generic.so\n");

    EXPECT_EQ(path, "/usr/lib/x86_64-linux-gnu/libQt6WaylandClient.so.6.8.2");
}

TEST(QtHardwareIntegrationTest, IgnoresQt5)
{
    const std::string path = parse(
            "7f40a000-7f40b000 r-xp 00001000 fe:03 1313 "
            "/usr/lib/x86_64-linux-gnu/libQt5WaylandClient.so.5.15.15\n");

    EXPECT_TRUE(path.empty());
}

// A process started before a Qt upgrade still maps the replaced file; the suffix
// keeps its path from resolving to the host's current library.
TEST(QtHardwareIntegrationTest, DeletedLibraryKeepsItsSuffix)
{
    const std::string path = parse(
            "7f40a000-7f40b000 r-xp 00001000 fe:03 1313 "
            "/usr/lib/x86_64-linux-gnu/libQt6WaylandClient.so.6.8.2 (deleted)\n");

    EXPECT_TRUE(path.ends_with(" (deleted)"));
}

// This test process links no QtWaylandClient, like any non-Qt client.
TEST(QtHardwareIntegrationTest, ReadsPluginQtVersions)
{
    ASSERT_EQ(kMetaData.size(), 0xc1u);
    const auto versions = qtPluginVersions(elfWithNotes(
            note("GNU", NT_GNU_BUILD_ID, std::string(20, '\x42'))
            + note("qt-project!", 0x74510001, kMetaData)));

    EXPECT_EQ(versions.gui, "6.8.2");
    EXPECT_EQ(versions.waylandClient, "6.8.2");
}

TEST(QtHardwareIntegrationTest, PluginWithoutMetaDataHasNoVersions)
{
    EXPECT_TRUE(qtPluginVersions(elfWithNotes(note("qt-project!", 1, kMetaData))).gui.empty());
    EXPECT_TRUE(qtPluginVersions(elfWithNotes(note("qt-project!", 0x74510001,
                                                   kMetaData.substr(0, 100))))
                        .gui.empty());
    EXPECT_TRUE(qtPluginVersions("not an elf file").gui.empty());
}

TEST(QtHardwareIntegrationTest, ClientWithoutQtWaylandClientIsRefused)
{
    wl_display* display = wl_display_create();
    ASSERT_TRUE(display);
    int fds[2] = {-1, -1};
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
    wl_client* client = wl_client_create(display, fds[0]);
    ASSERT_TRUE(client);

    EXPECT_FALSE(canLoadCutefishEgl(client, false));

    wl_client_destroy(client);
    close(fds[1]);
    wl_display_destroy(display);
}

} // namespace
} // namespace cutefish::wayland
