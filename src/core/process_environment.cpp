// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/process_environment.h"

#include <cstdlib>
#include <map>
#include <optional>

#include <unistd.h>

namespace cutefish {

namespace {

// What each private variable was before the compositor set it; empty when it
// was not set at all.
std::map<std::string, std::optional<std::string>, std::less<>> &originals()
{
    static std::map<std::string, std::optional<std::string>, std::less<>> values;
    return values;
}

} // namespace

void setPrivateEnvironment(const char *name, std::string_view value)
{
    if (!originals().count(name)) {
        const char *original = std::getenv(name);
        originals()[name] = original ? std::optional<std::string>(original) : std::nullopt;
    }
    setenv(name, std::string(value).c_str(), 1);
}

std::vector<std::string> childEnvironment()
{
    std::vector<std::string> environment;
    for (char **entry = environ; *entry; ++entry) {
        const std::string_view text(*entry);
        const auto separator = text.find('=');
        if (separator != std::string_view::npos
            && originals().count(text.substr(0, separator)))
            continue;
        environment.emplace_back(text);
    }
    for (const auto &[name, value] : originals()) {
        if (value)
            environment.push_back(name + '=' + *value);
    }
    return environment;
}

} // namespace cutefish
