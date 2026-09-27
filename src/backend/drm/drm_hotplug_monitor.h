// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/host/drm_display_host_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: monitor DRM udev events in Qt and coalesce dock bursts
// behind a bounded reprobe deadline.

#pragma once

#include <memory>

#include <QObject>
#include <QTimer>

struct udev;
struct udev_monitor;
class QSocketNotifier;

namespace cutefish::backend {

class DrmHotplugMonitor final : public QObject
{
    Q_OBJECT

public:
    explicit DrmHotplugMonitor(QObject *parent = nullptr);
    ~DrmHotplugMonitor() override;

    bool start();
    void notifyForTest();

signals:
    void snapshotsInvalidated();
    void reprobeRequested();

private:
    void onReadable();
    void schedule();
    void fire();

    udev *m_udev = nullptr;
    udev_monitor *m_monitor = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_quiet;
    QTimer m_deadline;
};

} // namespace cutefish::backend
