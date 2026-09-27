// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WM_QUIT_ALL_H_
#define CUTEFISH_WM_QUIT_ALL_H_

#include <functional>
#include <map>
#include <optional>

#include <QObject>
#include <QString>
#include <QTimer>

namespace cutefish::wm {

class Window;
class WindowManager;

// Before the session ends, every application is asked to quit as Quit asks one,
// and the session waits for them. An application asking about unsaved work is
// waited for however long the user takes; one that dismissed its prompt and kept
// its window cancelled, and one that neither closes nor asks stopped responding.
class QuitAll : public QObject
{
    Q_OBJECT

public:
    enum class Outcome {
        Quit,
        Cancelled,
        Unresponsive,
    };

    struct Result {
        Outcome outcome = Outcome::Quit;
        // The application that kept the session from ending.
        QString applicationId;
    };

    // How long a window may stay after its prompt closed, time enough to save.
    static constexpr int kCancelledAfterMs = 2000;
    static constexpr int kUnresponsiveAfterMs = 10000;

    explicit QuitAll(WindowManager *manager, std::function<qint64()> clock = {},
                     QObject *parent = nullptr);

    // False while a request is already running.
    bool start(std::function<void(const Result &)> done);

    bool isRunning() const { return static_cast<bool>(m_done); }

    // Looks at the windows again. Runs on a timer and on every window removed.
    void update();

private:
    struct Asked {
        qint64 at = 0;
        std::optional<qint64> promptSeen;
    };

    bool isApplicationWindow(const Window *window) const;
    void ask(Window *window, qint64 now);
    void finish(const Result &result);

    WindowManager *m_manager;
    std::function<qint64()> m_clock;
    std::function<void(const Result &)> m_done;
    // By window id, which is never reused.
    std::map<quint64, Asked> m_asked;
    QTimer m_timer;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_QUIT_ALL_H_
