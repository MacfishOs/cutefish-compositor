// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "display/display_identity.h"

#include <QHash>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace cutefish::display {
namespace {
QString baseKey(const DisplaySnapshot &display)
{
    if (display.connectionType == DisplayConnectionType::Virtual)
        return QStringLiteral("virtual:") + display.portKey;
    return display.edidKey.isEmpty() ? QStringLiteral("port:") + display.portKey
                                     : QStringLiteral("edid:") + display.edidKey;
}
} // namespace

void DisplayIdentity::rememberPersistentKeys(const QStringList &keys)
{
    for (const auto &key : keys) {
        if (key.startsWith(QStringLiteral("edid:")) && key.contains(QLatin1Char('@')))
            m_ambiguous.insert(key.section(QLatin1Char('@'), 0, 0));
    }
}

void DisplayIdentity::assign(QList<DisplaySnapshot> &displays)
{
    std::sort(displays.begin(), displays.end(), [](const auto &a, const auto &b) {
        return a.portKey < b.portKey;
    });
    QHash<QString, int> counts;
    for (const auto &display : displays)
        ++counts[baseKey(display)];
    QSet<DisplayId> used;
    for (auto &display : displays) {
        const QString base = baseKey(display);
        bool duplicate = counts.value(base) > 1 || m_ambiguous.contains(base);
        for (const auto &known : m_known)
            duplicate |= baseKey(known) == base
                    && known.persistentKey.startsWith(base + QLatin1Char('@'));
        if (duplicate)
            m_ambiguous.insert(base);
        display.persistentKey = duplicate ? base + QLatin1Char('@') + display.portKey : base;
        const DisplaySnapshot *match = nullptr;
        for (const auto &known : m_known) {
            if (baseKey(known) == base && known.portKey == display.portKey
                && !used.contains(known.sessionId)) {
                match = &known;
                break;
            }
        }
        if (!match && !duplicate) {
            for (const auto &known : m_known) {
                if (baseKey(known) == base && !used.contains(known.sessionId)) {
                    match = &known;
                    break;
                }
            }
        }
        display.sessionId = match ? match->sessionId
                                  : DisplayId(QStringLiteral("output-")
                                              + QUuid::createUuid().toString(QUuid::WithoutBraces));
        used.insert(display.sessionId);
    }
    for (const auto &display : displays) {
        auto it = std::find_if(m_known.begin(), m_known.end(), [&](const auto &known) {
            return known.sessionId == display.sessionId;
        });
        if (it == m_known.end())
            m_known.append(display);
        else
            *it = display;
    }
}
} // namespace cutefish::display
