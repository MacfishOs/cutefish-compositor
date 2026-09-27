// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "display/edid.h"

#include <QCryptographicHash>

namespace cutefish::display {

EdidInfo parseEdid(const QByteArray &edid)
{
    EdidInfo info;
    if (edid.size() < 128 || edid.left(8) != QByteArray::fromHex("00ffffffffffff00"))
        return info;
    uint checksum = 0;
    for (int i = 0; i < 128; ++i)
        checksum += static_cast<unsigned char>(edid.at(i));
    if (checksum & 255)
        return info;

    QByteArray key = edid.mid(8, 8);
    for (int offset = 54; offset + 18 <= 126; offset += 18) {
        if (edid.mid(offset, 4) == QByteArray::fromHex("000000ff"))
            key += edid.mid(offset + 5, 13).trimmed();
        // Monitor name, read as ui/display/util/edid_parser.cc does.
        if (edid.mid(offset, 5) == QByteArray::fromHex("000000fc00"))
            info.model = QString::fromLatin1(edid.mid(offset + 5, 13)).trimmed();
    }
    info.key = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());

    // Three 5-bit letters, big endian, 1 for 'A'.
    const uint manufacturer = (uint(uchar(edid.at(8))) << 8) | uchar(edid.at(9));
    if (manufacturer) {
        for (int shift : {10, 5, 0})
            info.make += QLatin1Char(char('A' - 1 + ((manufacturer >> shift) & 31)));
    }
    return info;
}

} // namespace cutefish::display
