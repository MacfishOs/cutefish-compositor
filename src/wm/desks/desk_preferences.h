// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/wm/desks/desks_restore_util.h
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: the `cutefish/desks` settings file instead of the
// primary user's prefs, and only what outlives the windows - how many desks
// there are, the names the user gave them and which one was active. Windows
// are not restored onto desks.

#ifndef CUTEFISH_WM_DESKS_DESK_PREFERENCES_H_
#define CUTEFISH_WM_DESKS_DESK_PREFERENCES_H_

#include <QSettings>

#include "wm/desks/desks_controller.h"

namespace cutefish::wm {

class DeskPreferences : public DesksObserver
{
public:
    // Restores the desks, then keeps the file up to date.
    explicit DeskPreferences(DesksController *desks);
    DeskPreferences(const DeskPreferences &) = delete;
    DeskPreferences &operator=(const DeskPreferences &) = delete;
    ~DeskPreferences() override;

    void onDeskAdded(const Desk *) override { save(); }
    void onDeskRemoved(const Desk *) override { save(); }
    void onDeskActivationChanged(const Desk *, const Desk *) override { save(); }
    void onDeskNameChanged(const Desk *) override { save(); }

private:
    void restore();
    void save();

    DesksController *m_desks = nullptr;
    QSettings m_settings;
    bool m_restoring = false;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_DESKS_DESK_PREFERENCES_H_
