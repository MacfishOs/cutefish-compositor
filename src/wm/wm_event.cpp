// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/wm_event.h"

namespace cutefish::wm {

WMEvent::WMEvent(WMEventType type)
    : m_type(type)
{
}

WMEvent::~WMEvent() = default;

bool WMEvent::isTransitionEvent() const
{
    switch (m_type) {
    case WMEventType::Normal:
    case WMEventType::Maximize:
    case WMEventType::Minimize:
    case WMEventType::Fullscreen:
    case WMEventType::SnapPrimary:
    case WMEventType::SnapSecondary:
    case WMEventType::Restore:
        return true;
    default:
        return false;
    }
}

bool WMEvent::isCompoundEvent() const
{
    return m_type == WMEventType::ToggleMaximize || m_type == WMEventType::ToggleFullscreen;
}

bool WMEvent::isBoundsEvent() const
{
    return m_type == WMEventType::SetBounds;
}

SetBoundsWMEvent::SetBoundsWMEvent(const QRect &bounds)
    : WMEvent(WMEventType::SetBounds)
    , m_requestedBounds(bounds)
{
}

SetBoundsWMEvent::~SetBoundsWMEvent() = default;

} // namespace cutefish::wm
