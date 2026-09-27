// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "surface_association.h"

#include <QDebug>

namespace cutefish::xwayland {

bool SurfaceAssociation::addSurface(uint64_t serial, Surface* surface)
{
    if (!serial || !surface || m_surfaceSerials.size() >= limit ||
        !m_surfaceSerials.insert(serial).second)
        return false;
    m_surfaces[serial] = surface;
    match(serial);
    return true;
}

bool SurfaceAssociation::addWindow(uint64_t serial, uint32_t window)
{
    if (!serial || !window || m_windowSerials.size() >= limit ||
        !m_windowSerials.insert(serial).second)
        return false;
    removeWindow(window);
    m_windows[serial] = window;
    match(serial);
    return true;
}

void SurfaceAssociation::match(uint64_t serial)
{
    auto s = m_surfaces.find(serial);
    auto w = m_windows.find(serial);
    if (s == m_surfaces.end() || w == m_windows.end())
        return;
    auto* surface = s->second;
    auto window = w->second;
    m_surfaces.erase(s);
    m_windows.erase(w);
    if (associated)
        associated(window, surface);
}

void SurfaceAssociation::removeSurface(Surface* surface)
{
    std::erase_if(m_surfaces, [surface](const auto& entry) {
        if (entry.second != surface)
            return false;
        qWarning() << "Xwayland abandoned surface serial" << entry.first;
        return true;
    });
}

void SurfaceAssociation::removeWindow(uint32_t window)
{
    std::erase_if(m_windows, [window](const auto& entry) {
        if (entry.second != window)
            return false;
        qWarning() << "Xwayland abandoned window serial" << entry.first;
        return true;
    });
}

} // namespace cutefish::xwayland
