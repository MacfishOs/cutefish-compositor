// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/desks/desks_restore_util.cc
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65

#include "wm/desks/desk_preferences.h"

#include <algorithm>

#include <QScopedValueRollback>
#include <QStringList>

#include "wm/desks/desk.h"

namespace cutefish::wm {

namespace {

const QString kCount = QStringLiteral("Count");
const QString kActive = QStringLiteral("Active");
// One entry per desk, empty for a desk with its default name.
const QString kNames = QStringLiteral("Names");

} // namespace

DeskPreferences::DeskPreferences(DesksController *desks)
    : m_desks(desks)
    , m_settings(QSettings::UserScope, QStringLiteral("cutefish"), QStringLiteral("desks"))
{
    restore();
    m_desks->addObserver(this);
}

DeskPreferences::~DeskPreferences()
{
    m_desks->removeObserver(this);
}

void DeskPreferences::restore()
{
    const QScopedValueRollback restoring(m_restoring, true);
    const int count =
            std::clamp(m_settings.value(kCount, 1).toInt(), 1, DesksController::kMaxDesks);
    const QStringList names = m_settings.value(kNames).toStringList();
    while (int(m_desks->desks().size()) < count)
        m_desks->newDesk(DesksCreationRemovalSource::DesksRestore);
    for (int index = 0; index < count && index < names.size(); ++index) {
        if (!names.at(index).isEmpty())
            m_desks->setDeskName(m_desks->deskAt(index), names.at(index));
    }
    if (Desk *active = m_desks->deskAt(m_settings.value(kActive, 0).toInt()))
        m_desks->activateDesk(active, DesksSwitchSource::DeskRestored);
}

void DeskPreferences::save()
{
    if (m_restoring)
        return;
    QStringList names;
    for (const auto &desk : m_desks->desks())
        names.append(desk->isNameSetByUser() ? desk->name() : QString());
    m_settings.setValue(kCount, int(m_desks->desks().size()));
    m_settings.setValue(kActive, m_desks->activeDeskIndex());
    m_settings.setValue(kNames, names);
}

} // namespace cutefish::wm
