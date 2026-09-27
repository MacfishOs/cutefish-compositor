// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/types/native_display_delegate.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: queue native operations on the Qt event loop while
// keeping the backend-independent completion interface.

#include "display/callback_native_display_delegate.h"
#include <QTimer>

namespace cutefish::display {

CallbackNativeDisplayDelegate::CallbackNativeDisplayDelegate(HardwareSnapshot snapshot,
                                                             ConfigureHandler configure,
                                                             PowerHandler power, QObject *parent)
    : NativeDisplayDelegate(parent)
    , m_snapshot(std::move(snapshot))
    , m_configure(std::move(configure))
    , m_power(std::move(power))
{
}

void CallbackNativeDisplayDelegate::invalidateSnapshots()
{
    ++m_epoch;
    emit hardwareInvalidated(m_snapshot.topologyGeneration + 1);
}

void CallbackNativeDisplayDelegate::setSnapshot(HardwareSnapshot snapshot)
{
    ++m_epoch;
    const uint64_t generation = snapshot.topologyGeneration;
    m_snapshot = std::move(snapshot);
    emit hardwareInvalidated(generation);
}

void CallbackNativeDisplayDelegate::takeDisplayControl(ControlCallback callback)
{
    callback(true);
}

void CallbackNativeDisplayDelegate::relinquishDisplayControl(ControlCallback callback)
{
    callback(true);
}

void CallbackNativeDisplayDelegate::queryHardware(QueryCallback callback)
{
    if (m_snapshot.isValid())
        callback(m_snapshot, {});
    else
        callback(std::nullopt,
                 {DisplayErrorCode::Unavailable,
                  QStringLiteral("Display hardware is unavailable"),
                  {}});
}

void CallbackNativeDisplayDelegate::configure(const DisplayConfiguration &configuration,
                                              ConfigureMode mode, ConfigureCallback callback)
{
    QTimer::singleShot(
            0, this,
            [this, configuration, mode, epoch = m_epoch, callback = std::move(callback)]() mutable {
                if (epoch != m_epoch) {
                    callback({false,
                              false,
                              {DisplayErrorCode::StaleConfiguration,
                               QStringLiteral("Display snapshots were invalidated"),
                               {}},
                              std::nullopt});
                    return;
                }
                callback(m_configure ? m_configure(configuration, mode)
                                     : ConfigurationResult {
                                               false,
                                               false,
                                               {DisplayErrorCode::Unavailable,
                                                QStringLiteral("No display backend is available"),
                                                {}},
                                               std::nullopt});
            });
}

void CallbackNativeDisplayDelegate::setDisplayPower(const QList<DisplayId> &displays,
                                                    PowerState state, ConfigureCallback callback)
{
    QTimer::singleShot(
            0, this,
            [this, displays, state, epoch = m_epoch, callback = std::move(callback)]() mutable {
                if (epoch != m_epoch) {
                    callback({false,
                              false,
                              {DisplayErrorCode::StaleConfiguration,
                               QStringLiteral("Display snapshots were invalidated"),
                               {}},
                              std::nullopt});
                    return;
                }
                callback(m_power ? m_power(displays, state)
                                 : ConfigurationResult {
                                           false,
                                           false,
                                           {DisplayErrorCode::UnsupportedProperty,
                                            QStringLiteral("Display power control is unavailable"),
                                            {}},
                                           std::nullopt});
            });
}

} // namespace cutefish::display
