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

SetBoundsWMEvent::SetBoundsWMEvent(const base::Rect &bounds)
    : WMEvent(WMEventType::SetBounds)
    , m_requestedBounds(bounds)
{
}

SetBoundsWMEvent::~SetBoundsWMEvent() = default;

} // namespace cutefish::wm
