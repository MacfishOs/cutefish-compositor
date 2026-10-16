// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_COMMON_MIME_ICONS_H_
#define CUTEFISH_SHELL_COMMON_MIME_ICONS_H_

#include <string>

namespace cutefish::shell {

// The icon name of a file's type, for the common cases: a folder; the type the
// shared-mime-info globs give its name, the heaviest and longest pattern
// first; else plain text or octet-stream by a look at its start. The icon is
// the "icons" file's, else the type with '/' as '-'. Touches the disk: the
// file thread's or a worker's.
std::string mimeIconNameForFile(const std::string &path);

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_COMMON_MIME_ICONS_H_
