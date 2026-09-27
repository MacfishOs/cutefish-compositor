// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/compositor_options.h"

#include <fcntl.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

namespace cutefish::core {

std::optional<CompositorOptions> CompositorOptions::parse(const QCoreApplication &app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Cutefish Wayland compositor"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption socketOption(QStringLiteral("socket"),
                                    QStringLiteral("Wayland socket name to listen on."),
                                    QStringLiteral("name"));
    parser.addOption(socketOption);
    QCommandLineOption drmDeviceOption(QStringLiteral("drm-device"),
                                       QStringLiteral("DRM device to drive, e.g. /dev/dri/card0."),
                                       QStringLiteral("path"));
    parser.addOption(drmDeviceOption);
    QCommandLineOption noDrmOption(QStringLiteral("no-drm"),
                                   QStringLiteral("Run the Wayland server only, without "
                                                  "acquiring a display."));
    parser.addOption(noDrmOption);
    QCommandLineOption headlessOutputsOption(
            QStringLiteral("headless-outputs"),
            QStringLiteral("Number of deterministic virtual outputs in --no-drm mode."),
            QStringLiteral("count"), QStringLiteral("1"));
    parser.addOption(headlessOutputsOption);
    QCommandLineOption wmDemoOption(QStringLiteral("wm-demo"),
                                    QStringLiteral("Enable development-only F1–F9 WM shortcuts."));
    parser.addOption(wmDemoOption);
    QCommandLineOption wmDemoScriptOption(
            QStringLiteral("wm-demo-script"),
            QStringLiteral("Development only: drive the window manager through a timed "
                           "sequence instead of waiting for the demo keys."));
    parser.addOption(wmDemoScriptOption);
    QCommandLineOption shellControlOption(
            QStringLiteral("shell-control-socket"),
            QStringLiteral("Path of the control socket the session passes trusted shell "
                           "connections over."),
            QStringLiteral("path"));
    parser.addOption(shellControlOption);
    QCommandLineOption loginOption(
            QStringLiteral("login"),
            QStringLiteral("Start the internal login screen for greetd session handoff."));
    parser.addOption(loginOption);
    QCommandLineOption readyFdOption(
            QStringLiteral("ready-fd"),
            QStringLiteral("Session startup notification file descriptor."), QStringLiteral("fd"));
    parser.addOption(readyFdOption);
    QCommandLineOption clipboardClientOption(
            QStringLiteral("clipboard-client"),
            QStringLiteral("Executable permitted to manage clipboard contents; may be repeated."),
            QStringLiteral("path"));
    parser.addOption(clipboardClientOption);
    QCommandLineOption captureClientOption(
            "capture-client", "Allow this executable to capture outputs (repeatable).", "path");
    parser.addOption(captureClientOption);
    QCommandLineOption inputMethodOption(
            QStringLiteral("input-method"),
            QStringLiteral("Development only: run this input-method desktop file id instead of "
                           "the saved selection, also without a display."),
            QStringLiteral("id"));
    parser.addOption(inputMethodOption);
    QCommandLineOption scaleOption(QStringLiteral("scale"),
                                   QStringLiteral("Output scale, a decimal from 1.0 through 4.0."),
                                   QStringLiteral("factor"), QStringLiteral("1.0"));
    parser.addOption(scaleOption);
    QCommandLineOption displayServiceOption(
            QStringLiteral("display-config-service"),
            QStringLiteral("Session D-Bus name for native display configuration."),
            QStringLiteral("name"),
            qEnvironmentVariable("CUTEFISH_DISPLAY_CONFIG_SERVICE",
                                 QStringLiteral("com.cutefish.DisplayConfig")));
    parser.addOption(displayServiceOption);
#ifdef CUTEFISH_ENABLE_XWAYLAND
    parser.addOption({"xwayland", "Enable the X11 frontend (enabled by default)."});
    parser.addOption({"no-xwayland", "Disable the X11 frontend; overrides --xwayland."});
    parser.addOption({"xwayland-shm", "Use Xwayland software buffers for diagnostics."});
#endif
    parser.addOption(
            {"xwayland-ready-file", "Publish DISPLAY/XAUTHORITY as JSON when available.", "path"});
    parser.process(app);

    CompositorOptions options;

    const auto scale = compositor::OutputScale::parse(parser.value(scaleOption));
    if (!scale) {
        qCritical("Invalid output scale; expected a decimal from 1.0 through 4.0");
        return std::nullopt;
    }
    options.scale = *scale;

    if (parser.isSet(readyFdOption)) {
        bool valid = false;
        options.readyFd = parser.value(readyFdOption).toInt(&valid);
        if (!valid || options.readyFd < 3 || fcntl(options.readyFd, F_SETFD, FD_CLOEXEC) < 0) {
            qCritical("Invalid session startup file descriptor");
            return std::nullopt;
        }
    }

    options.noDrm = parser.isSet(noDrmOption);
    if (options.noDrm) {
        bool valid = false;
        options.headlessOutputs = parser.value(headlessOutputsOption).toInt(&valid);
        if (!valid || options.headlessOutputs < 1 || options.headlessOutputs > 16) {
            qCritical("Invalid --headless-outputs count; expected 1 through 16");
            return std::nullopt;
        }
    }

    options.socketName = parser.value(socketOption);
    options.drmDevice = parser.value(drmDeviceOption);
    options.wmDemo = parser.isSet(wmDemoOption);
    options.wmDemoScript = parser.isSet(wmDemoScriptOption);
    options.shellControlSocket =
            parser.isSet(shellControlOption) ? parser.value(shellControlOption) : QString();
    options.login = parser.isSet(loginOption);
    options.clipboardClients = parser.values(clipboardClientOption);
    options.captureClients = parser.values(captureClientOption);
    options.inputMethodId =
            parser.isSet(inputMethodOption) ? parser.value(inputMethodOption) : QString();
    options.displayConfigService = parser.value(displayServiceOption);
#ifdef CUTEFISH_ENABLE_XWAYLAND
    options.xwayland = !parser.isSet(QStringLiteral("no-xwayland"));
    options.xwaylandSoftware = parser.isSet(QStringLiteral("xwayland-shm"));
#else
    options.xwayland = false;
#endif
    options.xwaylandReadyFile = parser.value(QStringLiteral("xwayland-ready-file"));

    return options;
}

} // namespace cutefish::core
