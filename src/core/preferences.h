// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_CORE_PREFERENCES_H_
#define CUTEFISH_CORE_PREFERENCES_H_

#include <QString>

namespace cutefish {

// ~/.config/cutefish/<process>.conf. Each file has one writer, the process it
// is named after; the compositor writes "compositor" and only reads the others.
QString preferencesFile(const QString &process);

} // namespace cutefish

#endif // CUTEFISH_CORE_PREFERENCES_H_
