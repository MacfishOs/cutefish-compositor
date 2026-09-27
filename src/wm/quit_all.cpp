// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/quit_all.h"

#include <algorithm>
#include <utility>

#include <QElapsedTimer>

#include "wm/window.h"
#include "wm/window_manager.h"

namespace cutefish::wm {

namespace {

constexpr int kPollMs = 100;

// A prompt of the application's own, not a menu or tooltip.
bool hasPrompt(const Window *window)
{
    for (const Window *child : window->transientChildren()) {
        if (!child->isPopup() || hasPrompt(child))
            return true;
    }
    return false;
}

} // namespace

QuitAll::QuitAll(WindowManager *manager, std::function<qint64()> clock, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
    , m_clock(std::move(clock))
{
    if (!m_clock) {
        m_clock = [] {
            static QElapsedTimer timer;
            if (!timer.isValid())
                timer.start();
            return timer.elapsed();
        };
    }
    m_timer.setInterval(kPollMs);
    connect(&m_timer, &QTimer::timeout, this, &QuitAll::update);
    // The last window going is when the session may end; no need to wait a poll.
    connect(m_manager, &WindowManager::windowRemoved, this, [this] {
        if (isRunning())
            QTimer::singleShot(0, this, &QuitAll::update);
    });
}

bool QuitAll::start(std::function<void(const Result &)> done)
{
    if (isRunning())
        return false;
    m_done = std::move(done);
    m_asked.clear();
    m_timer.start();
    update();
    return true;
}

bool QuitAll::isApplicationWindow(const Window *window) const
{
    return !window->transientParent() && !window->skipTaskbar();
}

void QuitAll::ask(Window *window, qint64 now)
{
    // Quit asks every window of the application at once; asking one of them
    // again would only raise a second prompt.
    for (const Window *candidate : m_manager->windows()) {
        if (isApplicationWindow(candidate) && m_manager->belongsToApplication(candidate, window))
            m_asked[candidate->id()] = Asked {now, std::nullopt};
    }
    m_manager->quitApplication(window);
}

void QuitAll::update()
{
    if (!isRunning())
        return;
    const qint64 now = m_clock();
    std::vector<Window *> windows;
    for (Window *window : m_manager->windows()) {
        if (isApplicationWindow(window))
            windows.push_back(window);
    }
    if (windows.empty()) {
        finish({Outcome::Quit, {}});
        return;
    }
    // Windows that opened since are asked too, on their own if their
    // application was already asked, so as not to raise its prompt again.
    for (Window *window : windows) {
        if (m_asked.contains(window->id()))
            continue;
        const bool askedBefore = std::any_of(windows.begin(), windows.end(), [&](Window *other) {
            return other != window && m_asked.contains(other->id())
                    && m_manager->belongsToApplication(window, other);
        });
        if (!askedBefore) {
            ask(window, now);
            continue;
        }
        m_asked[window->id()] = Asked {now, std::nullopt};
        if (window->canClose())
            window->requestClose();
    }
    for (Window *window : windows) {
        Asked &asked = m_asked[window->id()];
        if (hasPrompt(window)) {
            asked.promptSeen = now;
            continue;
        }
        if (asked.promptSeen ? now - *asked.promptSeen > kCancelledAfterMs
                             : now - asked.at > kUnresponsiveAfterMs) {
            finish({asked.promptSeen ? Outcome::Cancelled : Outcome::Unresponsive,
                    window->applicationId()});
            return;
        }
    }
}

void QuitAll::finish(const Result &result)
{
    m_timer.stop();
    m_asked.clear();
    // The callback may start another request.
    const auto done = std::exchange(m_done, nullptr);
    done(result);
}

} // namespace cutefish::wm
