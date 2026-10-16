// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_COMMON_APP_LAUNCHER_H_
#define CUTEFISH_SHELL_COMMON_APP_LAUNCHER_H_

#include <functional>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "shell/common/pending_calls.h"

namespace cutefish::base {
struct DesktopEntry;
} // namespace cutefish::base

namespace cutefish::shell {

// Starts applications the way every start in the session goes: through the
// session's AppRuntime (com.cutefish.AppRuntime), which puts each in a scope
// of its own, or here when the runtime does not answer. Nothing waits: the
// runtime's answer, or its absence, arrives later.
class ApplicationLauncher
{
public:
    using Done = std::function<void(bool started)>;

    explicit ApplicationLauncher(base::dbus::Connection *bus = base::dbus::Connection::session());

    // An xdg_activation token for the started application, so it may take
    // the keyboard. Unset, none is passed.
    std::function<std::string()> activationToken;

    // The entry by its id, as DesktopEntry::launch() did: the runtime starts
    // it and ties it to the entry; its command is the fallback.
    void launch(const base::DesktopEntry &entry, const std::vector<std::string> &arguments = {},
                Done done = {});
    // A command for the application `appId`.
    void launchCommand(const std::vector<std::string> &command, const std::string &workingDirectory,
                       const std::string &appId, Done done = {});
    // Asks the runtime to end what still runs of the application.
    void quitApplication(const std::string &appId);
    // Runs a program of the session's own, such as Settings, on its own.
    static bool startDetached(const std::string &program, const std::vector<std::string> &arguments = {});

private:
    base::dbus::Proxy m_runtime;
    PendingCalls m_calls;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_COMMON_APP_LAUNCHER_H_
