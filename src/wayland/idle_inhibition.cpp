// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/idle_inhibition.h"

namespace cutefish::wayland {
IdleInhibition::IdleInhibition(QObject *parent)
    : QObject(parent)
{
}

IdleInhibition::~IdleInhibition() = default;

void IdleInhibition::acquire()
{
    if (++m_count == 1 && !m_ignored)
        Q_EMIT inhibitedChanged();
}

void IdleInhibition::release()
{
    Q_ASSERT(m_count);
    if (--m_count == 0 && !m_ignored)
        Q_EMIT inhibitedChanged();
}

void IdleInhibition::setIgnored(bool ignored)
{
    const bool wasInhibited = inhibited();
    m_ignored = ignored;
    if (inhibited() != wasInhibited)
        Q_EMIT inhibitedChanged();
}
} // namespace cutefish::wayland
