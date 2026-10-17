// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/screenshot/screen_recording.h"

#include <chrono>
#include <format>
#include <utility>

#include "base/event_loop.h"
#include "base/process.h"
#include "base/strings.h"
#include "core/main_loop.h"

namespace cutefish::shell {

// Completing the file moves its index to the front, which reads it once more.
static constexpr std::chrono::milliseconds kFinishTimeout {30000};

ScreenRecording::ScreenRecording(std::string program, std::vector<std::string> environment)
    : m_program(std::move(program))
    , m_environment(std::move(environment))
{
    m_killTimer.setSingleShot(true);
    m_killTimer.onTimeout = [this] {
        if (m_process)
            m_process->kill();
    };
}

ScreenRecording::~ScreenRecording()
{
    if (m_process) {
        m_process->onFinished = nullptr;
        m_process->onStandardOutput = nullptr;
        m_process->closeStandardInput();
        m_process->waitForFinished(std::chrono::milliseconds(3000));
    }
}

std::vector<std::string> ScreenRecording::arguments(const Target &target, const std::string &path)
{
    std::vector<std::string> arguments;
    if (target.window) {
        arguments.push_back("--window");
        arguments.push_back("cutefish-" + std::to_string(target.window));
    } else {
        arguments.push_back("--output");
        arguments.push_back(target.output);
        if (!target.region.isEmpty()) {
            arguments.push_back("--region");
            arguments.push_back(std::format("{},{},{},{}", target.region.x(), target.region.y(),
                                            target.region.width(), target.region.height()));
        }
    }
    if (target.cursor)
        arguments.push_back("--cursor");
    if (!target.microphone.empty()) {
        arguments.push_back("--microphone");
        arguments.push_back(target.microphone);
    }
    arguments.push_back(path);
    return arguments;
}

bool ScreenRecording::start(const Target &target, const std::string &path)
{
    if (m_process)
        return false;

    m_path = path;
    m_output.clear();
    m_process = std::make_unique<base::Process>(core::mainLoop());
    if (!m_environment.empty())
        m_process->setEnvironment(m_environment);
    // The recorder stops at the end of its input: it stays open until stop().
    m_process->setStandardInputOpen(true);
    // Its loss ends only the recording.
    m_process->setOomProtectionDropped(true);
    m_process->onStandardOutput = [this](std::string_view output) { onOutput(output); };
    m_process->onFinished = [this](int exitCode, bool crashed) { onFinished(exitCode, crashed); };
    if (!m_process->start(m_program, arguments(target, path))) {
        m_process.reset();
        finished(false, "The screen recorder could not be started.");
        return true;
    }
    m_pid = m_process->pid();
    if (onStarted)
        onStarted(m_pid);
    return true;
}

void ScreenRecording::stop()
{
    if (!m_process || m_killTimer.isActive())
        return;
    m_process->writeStandardInput("stop\n");
    m_process->closeStandardInput();
    m_killTimer.start(kFinishTimeout);
}

void ScreenRecording::onOutput(std::string_view output)
{
    m_output += output;
    if (!m_recording && m_output.find("started") != std::string::npos) {
        m_recording = true;
        recordingChanged();
    }
}

void ScreenRecording::onFinished(int exitCode, bool crashed)
{
    if (!m_process)
        return;
    m_killTimer.stop();
    if (m_pid != 0 && onStopped)
        onStopped(std::exchange(m_pid, 0));
    m_pid = 0;
    const bool ok = !crashed && exitCode == 0;
    std::string error(base::strings::trimmed(m_process->standardError()));
    if (error.empty() && !ok)
        error = crashed ? "The screen recorder crashed." : "The screen recorder failed.";
    // Its last line says why.
    if (const size_t newline = error.rfind('\n'); newline != std::string::npos)
        error = error.substr(newline + 1);
    // Gone from inside its own callback: on the next loop turn.
    core::mainLoop()->post([process = std::shared_ptr<base::Process>(std::move(m_process))] {});

    const bool wasRecording = std::exchange(m_recording, false);
    if (wasRecording)
        recordingChanged();
    finished(ok, error);
}

} // namespace cutefish::shell
