// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "display/display_types.h"
#include <QSet>

namespace cutefish::display {

class DisplayIdentity
{
public:
    void seed(const QList<DisplaySnapshot> &displays) { m_known = displays; }

    void rememberPersistentKeys(const QStringList &keys);
    void assign(QList<DisplaySnapshot> &displays);

private:
    QList<DisplaySnapshot> m_known;
    QSet<QString> m_ambiguous;
};

} // namespace cutefish::display
