// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/host/drm_display_host_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: one udev monitor closes the subscribe-before-query race
// and turns a connector event burst into one bounded reprobe request.

#include "backend/drm/drm_hotplug_monitor.h"

#include <libudev.h>

#include <QSocketNotifier>

namespace cutefish::backend {

DrmHotplugMonitor::DrmHotplugMonitor(QObject *parent)
    : QObject(parent)
{
    m_quiet.setSingleShot(true);
    m_deadline.setSingleShot(true);
    connect(&m_quiet, &QTimer::timeout, this, &DrmHotplugMonitor::fire);
    connect(&m_deadline, &QTimer::timeout, this, &DrmHotplugMonitor::fire);
}

DrmHotplugMonitor::~DrmHotplugMonitor()
{
    delete m_notifier;
    if (m_monitor)
        udev_monitor_unref(m_monitor);
    if (m_udev)
        udev_unref(m_udev);
}

bool DrmHotplugMonitor::start()
{
    if (m_monitor)
        return true;
    m_udev = udev_new();
    if (!m_udev)
        return false;
    m_monitor = udev_monitor_new_from_netlink(m_udev, "udev");
    if (!m_monitor || udev_monitor_filter_add_match_subsystem_devtype(m_monitor, "drm", nullptr) < 0
        || udev_monitor_enable_receiving(m_monitor) < 0) {
        return false;
    }
    m_notifier = new QSocketNotifier(udev_monitor_get_fd(m_monitor), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &DrmHotplugMonitor::onReadable);
    return true;
}

void DrmHotplugMonitor::notifyForTest()
{
    schedule();
}

void DrmHotplugMonitor::onReadable()
{
    while (udev_device *device = udev_monitor_receive_device(m_monitor)) {
        udev_device_unref(device);
        schedule();
    }
}

void DrmHotplugMonitor::schedule()
{
    emit snapshotsInvalidated();
    m_quiet.start(100);
    if (!m_deadline.isActive())
        m_deadline.start(750);
}

void DrmHotplugMonitor::fire()
{
    if (!m_quiet.isActive() && !m_deadline.isActive())
        return;
    m_quiet.stop();
    m_deadline.stop();
    emit reprobeRequested();
}

} // namespace cutefish::backend
