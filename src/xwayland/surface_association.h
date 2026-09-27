// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>

namespace cutefish::compositor {
class Surface;
}

namespace cutefish::xwayland {

// One instance belongs to one Xwayland generation. Retired serials are never reused.
class SurfaceAssociation
{
public:
    using Surface = compositor::Surface;
    static constexpr size_t limit = 65536;

    static uint64_t serial(uint32_t low, uint32_t high)
    {
        return uint64_t(low) | (uint64_t(high) << 32);
    }

    bool addSurface(uint64_t serial, Surface* surface);
    bool addWindow(uint64_t serial, uint32_t window);
    void removeSurface(Surface* surface);
    void removeWindow(uint32_t window);
    std::function<void(uint32_t, Surface*)> associated;

    size_t pendingCount() const { return m_surfaces.size() + m_windows.size(); }

private:
    void match(uint64_t serial);
    std::map<uint64_t, Surface*> m_surfaces;
    std::map<uint64_t, uint32_t> m_windows;
    std::set<uint64_t> m_surfaceSerials;
    std::set<uint64_t> m_windowSerials;
};

} // namespace cutefish::xwayland
