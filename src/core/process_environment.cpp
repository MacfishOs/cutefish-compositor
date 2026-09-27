// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/process_environment.h"

#include <map>
#include <optional>

#include <QString>

namespace cutefish {

namespace {

// What each private variable was before the compositor set it; empty when it
// was not set at all.
std::map<QString, std::optional<QString>> &originals()
{
    static std::map<QString, std::optional<QString>> values;
    return values;
}

} // namespace

void setPrivateEnvironment(const char *name, const QByteArray &value)
{
    const QString key = QString::fromLatin1(name);
    if (!originals().count(key)) {
        originals()[key] = qEnvironmentVariableIsSet(name)
                ? std::optional<QString>(qEnvironmentVariable(name))
                : std::nullopt;
    }
    qputenv(name, value);
}

QProcessEnvironment childEnvironment()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &[name, value] : originals()) {
        if (value)
            environment.insert(name, *value);
        else
            environment.remove(name);
    }
    return environment;
}

} // namespace cutefish
