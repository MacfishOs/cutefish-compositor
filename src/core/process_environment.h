// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_CORE_PROCESS_ENVIRONMENT_H_
#define CUTEFISH_CORE_PROCESS_ENVIRONMENT_H_

#include <string>
#include <string_view>
#include <vector>

namespace cutefish {

// Sets `name` for the compositor's own Qt alone, as the offscreen platform its
// lock screen and switcher render on. Processes it starts from
// childEnvironment() get the value the compositor was started with instead.
void setPrivateEnvironment(const char *name, std::string_view value);

// The environment to start a child process from, as NAME=value entries: the
// compositor's, without its private settings.
std::vector<std::string> childEnvironment();

} // namespace cutefish

#endif // CUTEFISH_CORE_PROCESS_ENVIRONMENT_H_
