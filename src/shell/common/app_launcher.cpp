// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/common/app_launcher.h"

#include <chrono>

#include "base/desktop_entry.h"
#include "base/process.h"

namespace cutefish::shell {

// A runtime that stops answering must not leave the start hanging for the
// bus's half minute.
static constexpr std::chrono::seconds kRuntimeTimeout(5);

ApplicationLauncher::ApplicationLauncher(base::dbus::Connection *bus)
    : m_runtime(bus, "com.cutefish.AppRuntime", "/AppRuntime", "com.cutefish.AppRuntime")
{
    m_runtime.setTimeout(kRuntimeTimeout);
}

static bool startHere(const std::vector<std::string> &command, const std::string &workingDirectory)
{
    if (command.empty() || command.front().empty())
        return false;
    const std::vector<std::string> arguments(command.begin() + 1, command.end());
    return base::Process::startDetached(command.front(), arguments, workingDirectory) > 0;
}

void ApplicationLauncher::launch(const base::DesktopEntry &entry, const std::vector<std::string> &arguments,
                                 Done done)
{
    const std::vector<std::string> command = entry.commandForArguments(arguments);
    const std::string workingDirectory = entry.workingDirectory;
    const std::string id = entry.id;
    if (!m_runtime.isValid() || id.empty()) {
        launchCommand(command, workingDirectory, id, std::move(done));
        return;
    }

    m_calls.track([&](std::function<void()> finished) {
        return m_runtime.callAsync(
                "launchApplication",
                [this, command, workingDirectory, id, done, finished](const base::dbus::Reply &reply) {
                    if (reply.value<uint32_t>().value_or(0) != 0) {
                        if (done)
                            done(true);
                    } else {
                        launchCommand(command, workingDirectory, id, done);
                    }
                    finished();
                },
                id, arguments, std::string());
    });
}

void ApplicationLauncher::launchCommand(const std::vector<std::string> &command,
                                        const std::string &workingDirectory, const std::string &appId, Done done)
{
    if (command.empty() || command.front().empty()) {
        if (done)
            done(false);
        return;
    }
    if (!m_runtime.isValid()) {
        const bool started = startHere(command, workingDirectory);
        if (done)
            done(started);
        return;
    }

    m_calls.track([&](std::function<void()> finished) {
        return m_runtime.callAsync(
                "launchCommand",
                [command, workingDirectory, done, finished](const base::dbus::Reply &reply) {
                    const bool started =
                            reply.value<uint32_t>().value_or(0) != 0 || startHere(command, workingDirectory);
                    if (done)
                        done(started);
                    finished();
                },
                appId, command, workingDirectory, std::string());
    });
}

void ApplicationLauncher::quitApplication(const std::string &appId)
{
    if (!appId.empty())
        m_runtime.send("quitApplication", appId);
}

bool ApplicationLauncher::startDetached(const std::string &program, const std::vector<std::string> &arguments,
                                        const std::vector<std::string> &environment)
{
    return base::Process::startDetached(program, arguments, {}, environment) > 0;
}

} // namespace cutefish::shell
