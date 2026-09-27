// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/manager/display_manager.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: one Qt Core model owns immutable hardware snapshots
// and applied logical display batches without Ash observers or preferences.

#pragma once

#include <functional>
#include <optional>

#include <QObject>
#include <QPointF>
#include <QRect>

#include "display/display_transform.h"
#include "display/display_types.h"
#include "display/mirror_transform.h"

namespace cutefish::display {

class DisplayManager : public QObject
{
    Q_OBJECT

public:
    explicit DisplayManager(QObject *parent = nullptr);

    const std::optional<HardwareSnapshot> &hardware() const { return m_hardware; }

    const std::optional<AppliedConfiguration> &applied() const { return m_applied; }

    // The runtime publisher completes consumer updates before public observers run.
    void setPublisher(std::function<void()> publisher) { m_publisher = std::move(publisher); }

    bool isPublishing() const { return m_publishing; }

    static QPointF globalToSurfaceLocal(const QPointF &point, const QPointF &surfaceOrigin)
    {
        return point - surfaceOrigin;
    }

    static QRect surfaceLocalToGlobal(const QRect &rect, const QPoint &surfaceOrigin)
    {
        return rect.translated(surfaceOrigin);
    }

    QPointF normalizedToGlobal(const QPointF &point) const;
    // A point on the touchscreen, 0..1 across its panel in the panel's own
    // orientation, on the desktop. Null when no display shows it.
    std::optional<QPointF> touchscreenToGlobal(const QPointF &normalized) const;
    std::optional<QPointF> globalToOutputLocal(const DisplayId &id, const QPointF &point) const;
    std::optional<QPointF> outputLocalToGlobal(const DisplayId &id, const QPointF &point) const;

    bool installHardware(HardwareSnapshot snapshot, DisplayError *error = nullptr);
    bool installApplied(AppliedConfiguration configuration, DisplayError *error = nullptr);
    void clearApplied(uint64_t topologyGeneration);

    const LogicalDisplay *display(const DisplayId &id) const;
    const LogicalDisplay *primaryDisplay() const;
    const LogicalDisplay *displayAt(const QPointF &point) const;
    const LogicalDisplay *nearestDisplay(const QPointF &point) const;
    const LogicalDisplay *displayForRect(const QRect &rect) const;
    QRect desktopBounds() const;

    struct DestinationMapping {
        QRect logicalBounds;
        QRect nativeContent;
        double renderScale;
        DisplayRotation rotation;

        MirrorTransform transform() const { return {logicalBounds, nativeContent, rotation}; }
    };

    std::optional<DestinationMapping> mappingFor(const DisplayId &destination) const;
    std::optional<DisplayTransform> transformFor(const DisplayId &id) const;

signals:
    void displayChangeStarted();
    void displaysChanged(uint64_t topologyGeneration, uint64_t appliedRevision);

private:
    const DisplaySnapshot *physicalDisplay(const DisplayId &id) const;

    bool m_publishing = false;
    std::function<void()> m_publisher;
    std::optional<HardwareSnapshot> m_hardware;
    std::optional<AppliedConfiguration> m_applied;
};

} // namespace cutefish::display
