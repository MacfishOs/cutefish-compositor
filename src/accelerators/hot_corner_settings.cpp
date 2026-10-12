// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "accelerators/hot_corner_settings.h"

#include <algorithm>

#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

#include "accelerators/accelerator.h"
#include "core/preferences.h"

namespace cutefish::accelerators {
namespace {

constexpr auto service = "com.cutefish.HotCorners1";
constexpr auto path = "/com/cutefish/HotCorners1";

// The D-Bus names, also the stem of the preference keys.
constexpr std::array<std::pair<HotCorner, const char *>, 4> kCorners = {{
        {HotCorner::TopLeft, "top-left"},
        {HotCorner::TopRight, "top-right"},
        {HotCorner::BottomLeft, "bottom-left"},
        {HotCorner::BottomRight, "bottom-right"},
}};

// What a corner can require: AltGr is a level, not a modifier one holds.
constexpr uint32_t kCornerModifiers = ShiftDown | ControlDown | AltDown | MetaDown;

constexpr std::array<const char *, 4> kKeys = {"TopLeft", "TopRight", "BottomLeft", "BottomRight"};

size_t indexOf(HotCorner corner)
{
    return static_cast<size_t>(corner);
}

} // namespace

HotCornerSettings::HotCornerSettings(QObject *parent, const QString &settingsFile)
    : QObject(parent)
    , m_settings(settingsFile.isEmpty() ? preferencesFile(QStringLiteral("compositor"))
                                       : settingsFile,
                 QSettings::IniFormat)
{
    m_settings.beginGroup(QStringLiteral("HotCorners"));
    for (size_t i = 0; i < kKeys.size(); ++i) {
        const QString key = QLatin1String(kKeys[i]);
        HotCornerBinding &binding = m_bindings[i];
        binding.action = acceleratorActionFromName(m_settings.value(key).toString());
        if (binding.action)
            binding.modifiers = m_settings.value(key + QLatin1String("Modifiers"), 0).toUInt()
                    & kCornerModifiers;
    }
}

HotCornerSettings::~HotCornerSettings()
{
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QLatin1String(path));
        bus.unregisterService(QLatin1String(service));
    }
}

bool HotCornerSettings::registerService()
{
    if (m_registered)
        return true;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(path), this,
                            QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots
                                    | QDBusConnection::ExportAllSignals)) {
        qWarning() << "Cannot export hot corner settings:" << bus.lastError().message();
        return false;
    }
    if (!bus.registerService(QLatin1String(service))) {
        qWarning() << "Cannot register hot corner settings:" << bus.lastError().message();
        bus.unregisterObject(QLatin1String(path));
        return false;
    }
    m_registered = true;
    return true;
}

const HotCornerBinding &HotCornerSettings::binding(HotCorner corner) const
{
    return m_bindings[indexOf(corner)];
}

void HotCornerSettings::setCorner(const QString &corner, const QString &action, int modifiers)
{
    const auto it = std::find_if(kCorners.begin(), kCorners.end(), [&corner](const auto &entry) {
        return corner == QLatin1String(entry.second);
    });
    if (it == kCorners.end())
        return;
    HotCornerBinding binding;
    binding.action = acceleratorActionFromName(action);
    if (binding.action)
        binding.modifiers = uint32_t(modifiers) & kCornerModifiers;
    const size_t index = indexOf(it->first);
    if (m_bindings[index] == binding)
        return;
    m_bindings[index] = binding;

    const QString key = QLatin1String(kKeys[index]);
    if (binding.action) {
        m_settings.setValue(key, acceleratorActionName(*binding.action));
        m_settings.setValue(key + QLatin1String("Modifiers"), binding.modifiers);
    } else {
        m_settings.remove(key);
        m_settings.remove(key + QLatin1String("Modifiers"));
    }
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        qWarning() << "Cannot persist hot corner settings to" << m_settings.fileName();
    emit changed();
}

int HotCornerSettings::heldModifiers() const
{
    return m_modifierSource ? int(m_modifierSource() & kCornerModifiers) : 0;
}

QString HotCornerSettings::actionName(HotCorner corner) const
{
    const HotCornerBinding &binding = m_bindings[indexOf(corner)];
    return binding.action ? acceleratorActionName(*binding.action) : QString();
}

int HotCornerSettings::modifiers(HotCorner corner) const
{
    return int(m_bindings[indexOf(corner)].modifiers);
}

} // namespace cutefish::accelerators
