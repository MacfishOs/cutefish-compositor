// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_CORE_COMPOSITOR_OPTIONS_H_
#define CUTEFISH_CORE_COMPOSITOR_OPTIONS_H_

#include <optional>
#include <string>
#include <vector>

#include "compositor/output_scale.h"

namespace cutefish::core {

// The command line, parsed and validated once. Nothing downstream reads argv
// again, so every value here is already known good.
struct CompositorOptions {
    // Returns nothing and reports the reason when a value fails validation.
    // Exits the process for --help, --version and unknown options.
    static std::optional<CompositorOptions> parse(int argc, char **argv);

    std::string socketName;
    std::string drmDevice;
    bool noDrm = false;
    int headlessOutputs = 1;
    bool wmDemo = false;
    bool wmDemoScript = false;
    std::string shellControlSocket;
    bool login = false;
    // Session startup notification descriptor, -1 when the session passed none.
    int readyFd = -1;
    std::vector<std::string> clipboardClients;
    std::vector<std::string> captureClients;
    std::vector<std::string> computerControlClients;
    // Development override; empty means use the saved selection.
    std::string inputMethodId;
    compositor::OutputScale scale;
    double animationDurationScale = 1;
    std::string displayConfigService;
    bool xwayland = true;
    bool xwaylandSoftware = false;
    std::string xwaylandReadyFile;
    // base::logging rules, e.g. "cutefish.compositor.debug=true".
    std::string logRules;

    bool demoEnabled() const { return wmDemo || wmDemoScript; }
};

// The executables whose processes may capture outputs: the portal backend and
// those given with --capture-client. The screenshot tool and the recorder are
// not among them: anyone could run them.
std::vector<std::string> captureExecutables(const CompositorOptions &options);

} // namespace cutefish::core

#endif // CUTEFISH_CORE_COMPOSITOR_OPTIONS_H_
