// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_CORE_PROCESS_ENVIRONMENT_H_
#define CUTEFISH_CORE_PROCESS_ENVIRONMENT_H_

#include <QByteArray>
#include <QProcessEnvironment>

namespace cutefish {

// Sets `name` for the compositor's own Qt alone, as the offscreen platform its
// lock screen and switcher render on. Processes it starts from
// childEnvironment() get the value the compositor was started with instead.
void setPrivateEnvironment(const char *name, const QByteArray &value);

// The environment to start a child process from: the compositor's, without
// its private settings.
QProcessEnvironment childEnvironment();

} // namespace cutefish

#endif // CUTEFISH_CORE_PROCESS_ENVIRONMENT_H_
