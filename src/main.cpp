// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <csignal>
#include <memory>
#include <sys/signalfd.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QSocketNotifier>

#include "core/compositor_options.h"
#include "core/compositor_shell.h"
#include "core/logging.h"
#include "core/process_environment.h"
#include "core/scoped_fd.h"

namespace {

// Blocks SIGINT/SIGTERM/SIGHUP and returns a signalfd for them, so shutdown runs
// on the event loop rather than in a signal handler. SIGHUP is what the session's
// terminal sends when its leader exits; dying of it would skip the teardown that
// leaves the last frame on screen for the next DRM master.
int createSignalFd()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0)
        return -1;

    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

} // namespace

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    const cutefish::ScopedFd signalFdOwner(createSignalFd());
    const int signalFd = signalFdOwner.get();
    // Nothing here runs on a GMainLoop, and Qt's glib dispatcher reads the
    // clock for every source on every iteration: two thirds of a frame's
    // clock_gettime calls, each a syscall on a machine without a usable TSC.
    cutefish::setPrivateEnvironment("QT_NO_GLIB", "1");
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cutefish-compositor"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    const auto options = cutefish::core::CompositorOptions::parse(app);
    if (!options)
        return 1;

    auto shell = std::make_unique<cutefish::core::CompositorShell>(*options);
    if (!shell->init())
        return 1;

    QSocketNotifier *signalNotifier = nullptr;
    if (signalFd >= 0) {
        signalNotifier = new QSocketNotifier(signalFd, QSocketNotifier::Read, &app);
        QObject::connect(signalNotifier, &QSocketNotifier::activated, &app, [&] {
            signalfd_siginfo info;
            while (read(signalFd, &info, sizeof(info)) == sizeof(info))
                ;
            qCInfo(cfWayland) << "Shutting down";
            shell->stop();
            app.quit();
        });
    } else {
        qCWarning(cfWayland) << "Failed to create signalfd; Ctrl-C will not shut down cleanly";
    }

    qCInfo(cfWayland, "cutefish-compositor started, WAYLAND_DISPLAY=%s",
           qUtf8Printable(shell->socketName()));

    if (!shell->notifyReady())
        return 1;

    const int result = app.exec();
    shell.reset();

    if (signalNotifier)
        signalNotifier->setEnabled(false);
    return result;
}
