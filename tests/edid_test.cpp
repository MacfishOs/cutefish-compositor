// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "display/edid.h"

#include <QCryptographicHash>

using namespace cutefish::display;

namespace {

QByteArray descriptor(uchar tag, const QByteArray &text)
{
    QByteArray result = QByteArray::fromHex("000000") + char(tag) + '\0';
    result += (text + '\n').leftJustified(13, ' ', true);
    return result;
}

// A 128-byte base block from a Dell monitor, reduced to what the parser reads.
QByteArray edid(const QByteArray &name, bool validChecksum = true)
{
    QByteArray block = QByteArray::fromHex("00ffffffffffff00");
    block += QByteArray::fromHex("10ac7ea04c4d5a30"); // "DEL", product, serial number
    block += QByteArray(54 - block.size(), '\0');
    block += QByteArray::fromHex("565e00a0a0a029503020350055502100001a"); // Detailed timing.
    block += descriptor(0xff, "ABC1234");
    block += name.isNull() ? descriptor(0x10, {}) : descriptor(0xfc, name);
    block += descriptor(0x10, {});
    block += QByteArray(127 - block.size(), '\0');
    uint sum = 0;
    for (char byte : block)
        sum += uchar(byte);
    block += char((256 - sum % 256) % 256 + (validChecksum ? 0 : 1));
    return block;
}

} // namespace

TEST(Edid, ReadsManufacturerAndMonitorName)
{
    const auto info = parseEdid(edid("DELL U2720Q"));
    EXPECT_EQ(info.make, QStringLiteral("DEL"));
    EXPECT_EQ(info.model, QStringLiteral("DELL U2720Q"));
}

TEST(Edid, KeyCoversVendorProductSerialOnly)
{
    const QByteArray bytes = edid("DELL U2720Q");
    const QByteArray hashed = bytes.mid(8, 8) + "ABC1234";
    const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(hashed, QCryptographicHash::Sha256).toHex());
    EXPECT_EQ(parseEdid(bytes).key, expected);
    // The name is not part of the identity.
    EXPECT_EQ(parseEdid(edid("Other name")).key, expected);
}

TEST(Edid, PanelWithoutNameDescriptorHasEmptyModel)
{
    const auto info = parseEdid(edid(QByteArray()));
    EXPECT_EQ(info.make, QStringLiteral("DEL"));
    EXPECT_TRUE(info.model.isEmpty());
    EXPECT_FALSE(info.key.isEmpty());
}

TEST(Edid, RejectsBadChecksumShortBlockAndMissingHeader)
{
    EXPECT_TRUE(parseEdid(edid("DELL U2720Q", false)).key.isEmpty());
    EXPECT_TRUE(parseEdid(edid("DELL U2720Q").left(127)).make.isEmpty());
    QByteArray noHeader = edid("DELL U2720Q");
    noHeader[0] = 1;
    EXPECT_TRUE(parseEdid(noHeader).model.isEmpty());
}
