// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_COMMON_TEXT_MATCH_H_
#define CUTEFISH_SHELL_COMMON_TEXT_MATCH_H_

#include <string>
#include <string_view>

namespace cutefish::shell {

// Case-insensitive search: each character compared by its simple Unicode case
// folding, so "É" finds "é" and "Σ" finds "ς".
bool containsIgnoringCase(std::string_view text, std::string_view needle);

// Unicode lower case, not only ASCII.
std::string toLower(std::string_view text);

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_COMMON_TEXT_MATCH_H_
