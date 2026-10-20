// Copyright 2026 Jatin Chaudhary <chaudharyjatin116@proton.me>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/spotlight/spotlight.h"

#include "base/logging.h"

namespace cutefish::shell {

static const base::LogCategory cfSpotlight("cutefish.shell.spotlight");

Spotlight::Spotlight(base::dbus::Connection *bus)
    : m_bus(bus)
{
}

Spotlight::~Spotlight()
{
    if (m_object && m_bus)
        m_bus->releaseName("org.cutefish.Spotlight");
}

bool Spotlight::registerService()
{
    if (m_object)
        return true;
    if (!m_bus)
        return false;
    auto object = std::make_unique<base::dbus::Object>(m_bus, "/Spotlight", "org.cutefish.Spotlight");
    object->addMethod("show", [this] { show(); });
    object->addMethod("hide", [this] { hide(); });
    object->addMethod("toggle", [this] { toggle(); });
    if (!object->publish()
        || !m_bus->requestName("org.cutefish.Spotlight", base::dbus::Connection::DoNotQueue)) {
        CF_WARNING(cfSpotlight) << "Cannot own org.cutefish.Spotlight";
        return false;
    }
    m_object = std::move(object);
    return true;
}

void Spotlight::show()
{
    m_shown = true;
    if (showWindow)
        showWindow();
}

void Spotlight::hide()
{
    m_shown = false;
    if (hideWindow)
        hideWindow();
}

void Spotlight::toggle()
{
    m_shown ? hide() : show();
}

} // namespace cutefish::shell
