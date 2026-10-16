// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/process_environment.h"

#include <unistd.h>

namespace cutefish {

std::vector<std::string> childEnvironment()
{
    std::vector<std::string> environment;
    for (char **entry = environ; *entry; ++entry)
        environment.emplace_back(*entry);
    return environment;
}

} // namespace cutefish
