//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_H_
#define CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_H_

#include <functional>
#include <memory>
#include <string>

#include "base/dbus.h"

namespace cutefish::shell {

// Spotlight Search D-Bus service: org.cutefish.Spotlight at /Spotlight
class Spotlight
{
public:
    explicit Spotlight(base::dbus::Connection *bus);
    ~Spotlight();

    bool registerService();

    void show();
    void hide();
    void toggle();

    bool isShown() const { return m_shown; }

    std::function<void()> showWindow;
    std::function<void()> hideWindow;

private:
    base::dbus::Connection *m_bus;
    std::unique_ptr<base::dbus::Object> m_object;
    bool m_shown = false;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SPOTLIGHT_SPOTLIGHT_H_
