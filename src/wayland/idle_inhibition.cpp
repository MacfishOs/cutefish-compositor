// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/idle_inhibition.h"

#include <cassert>

namespace cutefish::wayland {

void IdleInhibition::acquire()
{
    if (++m_count == 1 && !m_ignored)
        inhibitedChanged();
}

void IdleInhibition::release()
{
    assert(m_count);
    if (--m_count == 0 && !m_ignored)
        inhibitedChanged();
}

void IdleInhibition::setIgnored(bool ignored)
{
    const bool wasInhibited = inhibited();
    m_ignored = ignored;
    if (inhibited() != wasInhibited)
        inhibitedChanged();
}
} // namespace cutefish::wayland
