// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_COMMON_SERVICE_OWNER_H_
#define CUTEFISH_SHELL_COMMON_SERVICE_OWNER_H_

#include <functional>
#include <string>
#include <utility>

#include "base/dbus.h"

namespace cutefish::shell {

// The session's own services (org.cutefish.Services and its Battery,
// Brightness, Language, Appearance objects) start after the compositor and
// are not D-Bus activatable: a client's first read fails and nothing tells it
// to read again. `appeared` runs each time `name` gets an owner, so the
// client reads then. System services (UPower, power profiles, accounts,
// BlueZ, Privacy) are activatable and need none of this.
inline base::dbus::NameWatch followServiceOwner(base::dbus::Connection *bus, const std::string &name,
                                                std::function<void()> appeared)
{
    if (!bus)
        return {};
    return bus->watchName(name, [appeared = std::move(appeared)](const std::string &owner) {
        if (!owner.empty())
            appeared();
    });
}

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_COMMON_SERVICE_OWNER_H_
