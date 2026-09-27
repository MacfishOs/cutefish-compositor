// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "backend/drm/drm_properties.h"

#include <xf86drmMode.h>

namespace cutefish::backend {

DrmPropertyMap DrmPropertyMap::fromObject(int fd, uint32_t objectId, uint32_t objectType)
{
    DrmPropertyMap map;

    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, objectId, objectType);
    if (!props)
        return map;

    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            continue;
        map.insert(QByteArray(prop->name), prop->prop_id, props->prop_values[i]);
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return map;
}

void DrmPropertyMap::insert(const QByteArray& name, uint32_t id, uint64_t value)
{
    m_properties.insert(name, Property{id, value});
}

std::optional<DrmPropertyMap::Property> DrmPropertyMap::find(const QByteArray& name) const
{
    const auto it = m_properties.constFind(name);
    if (it == m_properties.constEnd())
        return std::nullopt;
    return *it;
}

uint32_t DrmPropertyMap::id(const QByteArray& name) const
{
    const auto it = m_properties.constFind(name);
    return it == m_properties.constEnd() ? 0 : it->id;
}

uint64_t DrmPropertyMap::value(const QByteArray& name, uint64_t fallback) const
{
    const auto it = m_properties.constFind(name);
    return it == m_properties.constEnd() ? fallback : it->value;
}

QList<QByteArray> DrmPropertyMap::missing(const QList<QByteArray>& required) const
{
    QList<QByteArray> result;
    for (const QByteArray& name : required) {
        if (!m_properties.contains(name))
            result.append(name);
    }
    return result;
}

} // namespace cutefish::backend
