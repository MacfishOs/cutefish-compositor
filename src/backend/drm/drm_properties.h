// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_BACKEND_DRM_DRM_PROPERTIES_H_
#define CUTEFISH_BACKEND_DRM_DRM_PROPERTIES_H_

#include <QByteArray>
#include <QHash>
#include <QList>

#include <cstdint>
#include <optional>

namespace cutefish::backend {

// Property ids are per-object and driver-assigned, so every atomic property has
// to be looked up by name before it can be set.
class DrmPropertyMap
{
public:
    struct Property {
        uint32_t id = 0;
        uint64_t value = 0;
    };

    DrmPropertyMap() = default;

    // Reads all properties of a DRM object. `objectType` is one of
    // DRM_MODE_OBJECT_*. Returns an empty map on failure.
    static DrmPropertyMap fromObject(int fd, uint32_t objectId, uint32_t objectType);

    void insert(const QByteArray& name, uint32_t id, uint64_t value);

    bool contains(const QByteArray& name) const { return m_properties.contains(name); }
    std::optional<Property> find(const QByteArray& name) const;

    // Zero when the property is absent; zero is never a valid property id.
    uint32_t id(const QByteArray& name) const;
    uint64_t value(const QByteArray& name, uint64_t fallback = 0) const;

    // Names from `required` that this object does not expose.
    QList<QByteArray> missing(const QList<QByteArray>& required) const;

    int size() const { return m_properties.size(); }
    bool isEmpty() const { return m_properties.isEmpty(); }

private:
    QHash<QByteArray, Property> m_properties;
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_DRM_DRM_PROPERTIES_H_
