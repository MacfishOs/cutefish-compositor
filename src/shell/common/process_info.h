// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_COMMON_PROCESS_INFO_H_
#define CUTEFISH_SHELL_COMMON_PROCESS_INFO_H_

#include <cstdint>
#include <string>

namespace cutefish::shell {

// The program a process runs, as /proc says it, for matching processes to
// their desktop entries. procfs is answered from memory, so these never wait
// on a disk.
struct ProcessCommand {
    std::string program;
    // The program's file name.
    std::string name;
};

// Empty when /proc cannot say. A script started through its shebang runs as
// the interpreter, which would match every application written in that
// language: the script stands for it instead, and inline code (-c, -e) or a
// module (-m) for nothing.
ProcessCommand processCommand(int64_t pid);

// The parent process, or 0.
int64_t parentProcess(int64_t pid);

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_COMMON_PROCESS_INFO_H_
