// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/preferences.h"

#include <QStandardPaths>

namespace cutefish {

QString preferencesFile(const QString &process)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
            + QStringLiteral("/cutefish/") + process + QStringLiteral(".conf");
}

} // namespace cutefish
