// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_CORE_COMPOSITOR_OPTIONS_H_
#define CUTEFISH_CORE_COMPOSITOR_OPTIONS_H_

#include <optional>

#include <QString>
#include <QStringList>

#include "compositor/output_scale.h"

class QCoreApplication;

namespace cutefish::core {

// The command line, parsed and validated once. Nothing downstream reads argv
// again, so every value here is already known good.
struct CompositorOptions {
    // Returns nothing and reports the reason when a value fails validation.
    // Exits the process through QCommandLineParser for --help and --version.
    static std::optional<CompositorOptions> parse(const QCoreApplication &app);

    QString socketName;
    QString drmDevice;
    bool noDrm = false;
    int headlessOutputs = 1;
    bool wmDemo = false;
    bool wmDemoScript = false;
    QString shellControlSocket;
    bool login = false;
    // Session startup notification descriptor, -1 when the session passed none.
    int readyFd = -1;
    QStringList clipboardClients;
    QStringList captureClients;
    // Development override; empty means use the saved selection.
    QString inputMethodId;
    compositor::OutputScale scale;
    QString displayConfigService;
    bool xwayland = true;
    bool xwaylandSoftware = false;
    QString xwaylandReadyFile;

    bool demoEnabled() const { return wmDemo || wmDemoScript; }
};

} // namespace cutefish::core

#endif // CUTEFISH_CORE_COMPOSITOR_OPTIONS_H_
