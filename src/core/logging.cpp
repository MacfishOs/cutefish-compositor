// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/logging.h"

namespace cutefish::core {

void setLoggingRules(std::string_view rules)
{
    base::logging::setRules(rules);
}

} // namespace cutefish::core

const cutefish::core::LogCategory cfWayland("cutefish.wayland");
const cutefish::core::LogCategory cfSession("cutefish.session");
const cutefish::core::LogCategory cfDrm("cutefish.drm");
const cutefish::core::LogCategory cfRender("cutefish.render");
const cutefish::core::LogCategory cfScene("cutefish.scene");
const cutefish::core::LogCategory cfCompositor("cutefish.compositor");
const cutefish::core::LogCategory cfInput("cutefish.input");
const cutefish::core::LogCategory cfInputTransitions("cutefish.input.transitions");
const cutefish::core::LogCategory cfCursorTransitions("cutefish.cursor.transitions");
