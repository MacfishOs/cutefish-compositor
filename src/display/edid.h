// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QByteArray>
#include <QString>

namespace cutefish::display {

// What the first 128-byte EDID block says about a monitor. Everything is
// empty when the block is missing, has no EDID header or fails its checksum.
struct EdidInfo {
    // SHA-256 over the vendor/product/serial bytes and the serial string.
    QString key;
    // The three-letter PNP manufacturer ID, such as "DEL".
    QString make;
    // The monitor name descriptor; panels often leave it out.
    QString model;
};

EdidInfo parseEdid(const QByteArray &edid);

} // namespace cutefish::display
