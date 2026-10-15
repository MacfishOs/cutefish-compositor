// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_CORE_PREFERENCES_H_
#define CUTEFISH_CORE_PREFERENCES_H_

#include <string>
#include <string_view>

namespace cutefish {

// ~/.config/cutefish, where every preference file of the session lives.
std::string preferencesDirectory();

// ~/.config/cutefish/<process>.conf. Each file has one writer, the process it
// is named after; the compositor writes "compositor" and only reads the others.
std::string preferencesFile(std::string_view process);

// Tests: preferences under `configHome`/cutefish instead of the user's;
// empty restores the user's. The test main sets a scratch one for every test.
void setConfigHomeForTesting(std::string configHome);

} // namespace cutefish

#endif // CUTEFISH_CORE_PREFERENCES_H_
