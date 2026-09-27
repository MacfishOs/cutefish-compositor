// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "logging.h"

// Keep startup and operational messages visible; routine window and input
// diagnostics require an explicit debug logging rule.
Q_LOGGING_CATEGORY(cfWayland, "cutefish.wayland", QtInfoMsg)
Q_LOGGING_CATEGORY(cfSession, "cutefish.session", QtInfoMsg)
Q_LOGGING_CATEGORY(cfDrm, "cutefish.drm", QtInfoMsg)
Q_LOGGING_CATEGORY(cfRender, "cutefish.render", QtInfoMsg)
Q_LOGGING_CATEGORY(cfScene, "cutefish.scene", QtInfoMsg)
Q_LOGGING_CATEGORY(cfCompositor, "cutefish.compositor", QtInfoMsg)
Q_LOGGING_CATEGORY(cfInput, "cutefish.input", QtInfoMsg)

// Opt-in button/capture transitions; no pointer-motion or keyboard text logging.
Q_LOGGING_CATEGORY(cfInputTransitions, "cutefish.input.transitions", QtInfoMsg)

Q_LOGGING_CATEGORY(cfCursorTransitions, "cutefish.cursor.transitions", QtInfoMsg)
