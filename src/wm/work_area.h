// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WM_WORK_AREA_H_
#define CUTEFISH_WM_WORK_AREA_H_

#include <map>

#include <QRect>
#include <QString>

#include "wm/types.h"

namespace cutefish::wm {

// Aggregates the work-area reservations the trusted shell makes through
// cutefish_shell_surface_v1.set_exclusive_zone.
//
// Keys are opaque tokens owned by the caller - the protocol layer uses the
// shell surface's address - so a reservation can be dropped again when the
// object that made it goes away, including when the whole shell disconnects.
class ExclusiveZones
{
public:
    struct Zone {
        ScreenEdge edge = ScreenEdge::Top;
        int thickness = 0;
        QString outputName;
    };

    // Returns true when the aggregate changed and the work area has to be
    // recomputed.
    bool set(const void* token, ScreenEdge edge, int thickness,
             const QString& outputName = QString());
    bool clear(const void* token);
    // Drops every reservation at once; what a shell disconnect does.
    bool clearAll();

    bool isEmpty() const { return m_zones.empty(); }

    // `outputBounds` minus every reservation. Reservations on the same edge do
    // not stack: the largest one wins, so a dock that briefly has two
    // reservations in flight cannot shrink the work area twice.
    QRect apply(const QRect& outputBounds) const;
    QRect apply(const QRect& outputBounds, const QString& outputName, bool primary) const;

private:
    std::map<const void*, Zone> m_zones;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_WORK_AREA_H_
