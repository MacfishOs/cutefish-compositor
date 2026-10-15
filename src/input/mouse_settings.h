// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <string>

#include "base/settings.h"
#include "base/signal.h"

namespace cutefish::input {

class DBusExport;

class MouseSettings
{
public:
    explicit MouseSettings(const std::string &settingsFile = {});
    ~MouseSettings();
    bool registerService();

    bool available() const { return m_available; }

    bool leftHanded() const { return m_leftHanded; }

    bool naturalScroll() const { return m_naturalScroll; }

    bool acceleration() const { return m_acceleration; }

    double pointerAcceleration() const { return m_pointerAcceleration; }

    void setAvailable(bool available);

    void setLeftHanded(bool enabled);
    void setNaturalScroll(bool enabled);
    void setAcceleration(bool enabled);
    void setPointerAcceleration(double value);

    base::Signal<> availableChanged;
    base::Signal<> leftHandedChanged;
    base::Signal<> naturalScrollChanged;
    base::Signal<> accelerationChanged;
    base::Signal<> pointerAccelerationChanged;
    base::Signal<> settingsChanged;

private:
    template<typename T>
    void save(const std::string &key, T value);
    void notify(const base::Signal<> &signal, const char *member);

    base::Settings m_settings;
    bool m_available = false;
    bool m_leftHanded = false;
    bool m_naturalScroll = true;
    bool m_acceleration = true;
    double m_pointerAcceleration = 0;
    std::unique_ptr<DBusExport> m_object;
};

} // namespace cutefish::input
