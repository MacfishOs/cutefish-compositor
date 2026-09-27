// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/manager/display_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: publish validated Qt value snapshots as one batch and
// centralize logical-display selection and coordinate lookup.

#include "display/display_manager.h"

#include <algorithm>
#include <limits>

#include <QScopedValueRollback>
#include <QSet>

namespace cutefish::display {

namespace {

void setError(DisplayError *target, DisplayErrorCode code, QString message,
              QList<DisplayId> displays = {})
{
    if (target)
        *target = {code, std::move(message), std::move(displays)};
}

bool containsHalfOpen(const QRect &bounds, const QPointF &point)
{
    return point.x() >= bounds.x() && point.y() >= bounds.y()
            && point.x() < bounds.x() + bounds.width() && point.y() < bounds.y() + bounds.height();
}

qint64 intersectionArea(const QRect &left, const QRect &right)
{
    const QRect intersection = left.intersected(right);
    return intersection.isEmpty() ? 0 : qint64(intersection.width()) * intersection.height();
}

double squaredDistance(const QRect &bounds, const QPointF &point)
{
    const double x =
            std::clamp(point.x(), double(bounds.x()), double(bounds.x() + bounds.width() - 1));
    const double y =
            std::clamp(point.y(), double(bounds.y()), double(bounds.y() + bounds.height() - 1));
    const double dx = point.x() - x;
    const double dy = point.y() - y;
    return dx * dx + dy * dy;
}

} // namespace

DisplayManager::DisplayManager(QObject *parent)
    : QObject(parent)
{
}

bool DisplayManager::installHardware(HardwareSnapshot snapshot, DisplayError *error)
{
    if (m_publishing) {
        setError(error, DisplayErrorCode::Busy,
                 QStringLiteral("Display publication is in progress"));
        return false;
    }
    QScopedValueRollback<bool> publishing(m_publishing, true);
    if (!snapshot.isValid()) {
        setError(error, DisplayErrorCode::Unavailable,
                 QStringLiteral("The hardware snapshot is invalid"));
        return false;
    }
    if (m_hardware && snapshot.compositorInstanceId == m_hardware->compositorInstanceId
        && snapshot.topologyGeneration <= m_hardware->topologyGeneration) {
        setError(error, DisplayErrorCode::StaleConfiguration,
                 QStringLiteral("The hardware snapshot generation did not advance"));
        return false;
    }

    emit displayChangeStarted();
    m_hardware = std::move(snapshot);
    if (m_applied
        && (m_applied->compositorInstanceId != m_hardware->compositorInstanceId
            || m_applied->topologyGeneration != m_hardware->topologyGeneration)) {
        m_applied.reset();
    }
    if (m_publisher)
        m_publisher();
    emit displaysChanged(m_hardware->topologyGeneration, m_applied ? m_applied->revision : 0);
    return true;
}

bool DisplayManager::installApplied(AppliedConfiguration configuration, DisplayError *error)
{
    if (m_publishing) {
        setError(error, DisplayErrorCode::Busy,
                 QStringLiteral("Display publication is in progress"));
        return false;
    }
    QScopedValueRollback<bool> publishing(m_publishing, true);
    if (!m_hardware || configuration.compositorInstanceId != m_hardware->compositorInstanceId
        || configuration.topologyGeneration != m_hardware->topologyGeneration
        || (m_applied && configuration.revision <= m_applied->revision)) {
        setError(error, DisplayErrorCode::StaleConfiguration,
                 QStringLiteral("The applied configuration does not match current hardware"));
        return false;
    }

    QSet<DisplayId> logicalIds;
    QSet<QString> waylandNames;
    QSet<DisplayId> destinations;
    int primaryCount = 0;
    for (const LogicalDisplay &logical : configuration.logicalDisplays) {
        if (!logical.id.isValid() || logical.bounds.width() <= 0 || logical.bounds.height() <= 0
            || logical.workArea.isEmpty() || !logical.bounds.contains(logical.workArea)
            || logical.destinations.isEmpty() || logical.waylandName.isEmpty()
            || logicalIds.contains(logical.id) || waylandNames.contains(logical.waylandName)) {
            setError(error, DisplayErrorCode::InvalidLayout,
                     QStringLiteral("An applied logical display is invalid"), {logical.id});
            return false;
        }
        for (const DisplayId &destination : logical.destinations) {
            if (!physicalDisplay(destination) || destinations.contains(destination)) {
                setError(error, DisplayErrorCode::UnknownOutput,
                         QStringLiteral("A physical destination is missing or duplicated"),
                         {destination});
                return false;
            }
            destinations.insert(destination);
        }
        logicalIds.insert(logical.id);
        waylandNames.insert(logical.waylandName);
        primaryCount += logical.primary ? 1 : 0;
    }
    for (int i = 0; i < configuration.logicalDisplays.size(); ++i) {
        for (int j = i + 1; j < configuration.logicalDisplays.size(); ++j) {
            if (configuration.logicalDisplays.at(i).bounds.intersects(
                        configuration.logicalDisplays.at(j).bounds)) {
                setError(error, DisplayErrorCode::InvalidLayout,
                         QStringLiteral("Applied logical displays overlap"),
                         {configuration.logicalDisplays.at(i).id,
                          configuration.logicalDisplays.at(j).id});
                return false;
            }
        }
    }
    if (!configuration.logicalDisplays.isEmpty() && primaryCount != 1) {
        setError(error, DisplayErrorCode::InvalidLayout,
                 QStringLiteral("Exactly one active logical display must be primary"));
        return false;
    }

    emit displayChangeStarted();
    m_applied = std::move(configuration);
    if (m_publisher)
        m_publisher();
    emit displaysChanged(m_applied->topologyGeneration, m_applied->revision);
    return true;
}

void DisplayManager::clearApplied(uint64_t topologyGeneration)
{
    if (m_publishing)
        return;
    QScopedValueRollback<bool> publishing(m_publishing, true);
    if (!m_applied && (!m_hardware || m_hardware->topologyGeneration == topologyGeneration))
        return;
    emit displayChangeStarted();
    m_applied.reset();
    if (m_publisher)
        m_publisher();
    emit displaysChanged(topologyGeneration, 0);
}

const LogicalDisplay *DisplayManager::display(const DisplayId &id) const
{
    if (!m_applied)
        return nullptr;
    for (const LogicalDisplay &logical : m_applied->logicalDisplays) {
        if (logical.id == id)
            return &logical;
    }
    return nullptr;
}

const LogicalDisplay *DisplayManager::primaryDisplay() const
{
    if (!m_applied)
        return nullptr;
    for (const LogicalDisplay &logical : m_applied->logicalDisplays) {
        if (logical.primary)
            return &logical;
    }
    return nullptr;
}

const LogicalDisplay *DisplayManager::displayAt(const QPointF &point) const
{
    if (!m_applied)
        return nullptr;
    for (const LogicalDisplay &logical : m_applied->logicalDisplays) {
        if (containsHalfOpen(logical.bounds, point))
            return &logical;
    }
    return nullptr;
}

const LogicalDisplay *DisplayManager::nearestDisplay(const QPointF &point) const
{
    if (const LogicalDisplay *exact = displayAt(point))
        return exact;
    if (!m_applied)
        return nullptr;

    const LogicalDisplay *nearest = nullptr;
    double best = std::numeric_limits<double>::max();
    for (const LogicalDisplay &logical : m_applied->logicalDisplays) {
        const double distance = squaredDistance(logical.bounds, point);
        if (!nearest || distance < best
            || (distance == best && logical.primary && !nearest->primary)) {
            nearest = &logical;
            best = distance;
        }
    }
    return nearest;
}

const LogicalDisplay *DisplayManager::displayForRect(const QRect &rect) const
{
    if (!m_applied || rect.isEmpty())
        return nearestDisplay(rect.center());

    const LogicalDisplay *selected = nullptr;
    qint64 best = -1;
    for (const LogicalDisplay &logical : m_applied->logicalDisplays) {
        const qint64 area = intersectionArea(logical.bounds, rect);
        if (!selected || area > best || (area == best && logical.primary && !selected->primary)) {
            selected = &logical;
            best = area;
        }
    }
    return best > 0 ? selected : nearestDisplay(rect.center());
}

QRect DisplayManager::desktopBounds() const
{
    QRect result;
    if (!m_applied)
        return result;
    for (const LogicalDisplay &logical : m_applied->logicalDisplays)
        result = result.united(logical.bounds);
    return result;
}

std::optional<DisplayManager::DestinationMapping>
DisplayManager::mappingFor(const DisplayId &destination) const
{
    if (!m_applied)
        return std::nullopt;
    const LogicalDisplay *logical = nullptr;
    for (const auto &candidate : m_applied->logicalDisplays) {
        if (candidate.destinations.contains(destination)) {
            logical = &candidate;
            break;
        }
    }
    const DisplaySnapshot *physical = nullptr;
    for (const auto &candidate : m_applied->physicalDisplays) {
        if (candidate.sessionId == destination) {
            physical = &candidate;
            break;
        }
    }
    if (!logical || !physical || !physical->currentMode)
        return std::nullopt;
    const QSize pixels = physical->currentMode->pixelSize;
    QSize oriented = pixels;
    const bool rotated = logical->rotation == DisplayRotation::Rotate90
            || logical->rotation == DisplayRotation::Rotate270;
    if (rotated)
        oriented.transpose();
    const QSize extent = logical->bounds.size();
    const double fit = logical->destinations.size() > 1
            ? std::min(double(oriented.width()) / extent.width(),
                       double(oriented.height()) / extent.height())
            : logical->scale.value();
    QSize content(qRound(extent.width() * fit), qRound(extent.height() * fit));
    if (rotated)
        content.transpose();
    const QRect native = logical->destinations.size() == 1
            ? QRect(QPoint(), pixels)
            : QRect(QPoint((pixels.width() - content.width()) / 2,
                           (pixels.height() - content.height()) / 2),
                    content);
    return DestinationMapping {logical->bounds, native, fit, logical->rotation};
}

std::optional<DisplayTransform> DisplayManager::transformFor(const DisplayId &id) const
{
    const LogicalDisplay *logical = display(id);
    if (!logical || logical->destinations.isEmpty())
        return std::nullopt;
    const DisplaySnapshot *physical = nullptr;
    for (const auto &candidate : m_applied->physicalDisplays) {
        if (candidate.sessionId == logical->destinations.first()) {
            physical = &candidate;
            break;
        }
    }
    if (!physical || !physical->currentMode)
        return std::nullopt;
    return DisplayTransform(logical->bounds.topLeft(), physical->currentMode->pixelSize,
                            logical->scale, logical->rotation);
}

QPointF DisplayManager::normalizedToGlobal(const QPointF &point) const
{
    const QRect bounds = desktopBounds();
    return QPointF(bounds.x() + point.x() * bounds.width(),
                   bounds.y() + point.y() * bounds.height());
}

std::optional<QPointF> DisplayManager::touchscreenToGlobal(const QPointF &normalized) const
{
    if (!m_applied || m_applied->logicalDisplays.isEmpty())
        return std::nullopt;
    // ash's TouchDeviceManager, reduced to its first rules: the touchscreen is
    // the internal panel's when that panel is shown, else the only display's,
    // else the primary's.
    const DisplaySnapshot *panel = nullptr;
    for (const DisplaySnapshot &physical : m_applied->physicalDisplays) {
        if (physical.connectionType == DisplayConnectionType::Internal
            && mappingFor(physical.sessionId)) {
            panel = &physical;
            break;
        }
    }
    if (!panel) {
        const LogicalDisplay *logical = m_applied->logicalDisplays.size() == 1
                ? &m_applied->logicalDisplays.front()
                : primaryDisplay();
        if (!logical || logical->destinations.isEmpty())
            return std::nullopt;
        for (const DisplaySnapshot &physical : m_applied->physicalDisplays) {
            if (physical.sessionId == logical->destinations.front())
                panel = &physical;
        }
    }
    if (!panel || !panel->currentMode)
        return std::nullopt;
    const auto mapping = mappingFor(panel->sessionId);
    if (!mapping)
        return std::nullopt;
    // The glass spans the panel's native mode. A smaller mode on an internal
    // panel goes through the GPU's panel fitter, which keeps the mode's aspect
    // ratio (amdgpu's and i915's default) and centres it between black bars.
    const QSize pixels = panel->currentMode->pixelSize;
    const QSizeF glass = panel->connectionType == DisplayConnectionType::Internal
                    && panel->nativeMode
            ? QSizeF(panel->nativeMode->pixelSize)
            : QSizeF(pixels);
    QRectF shown(QPointF(), QSizeF(pixels).scaled(glass, Qt::KeepAspectRatio));
    shown.moveCenter(QRectF(QPointF(), glass).center());
    const QPointF onShown(
            std::clamp((normalized.x() * glass.width() - shown.x()) / shown.width(), 0.0, 1.0),
            std::clamp((normalized.y() * glass.height() - shown.y()) / shown.height(), 0.0, 1.0));
    // The mode's pixels are where the mapping's transform starts; it takes
    // rotation and mirror letterboxing from there.
    const QPointF native(onShown.x() * (pixels.width() - 1e-3),
                         onShown.y() * (pixels.height() - 1e-3));
    return mapping->transform().nativeToLogical(native);
}

std::optional<QPointF> DisplayManager::globalToOutputLocal(const DisplayId &id,
                                                           const QPointF &point) const
{
    const auto *logical = display(id);
    return logical ? std::optional(point - logical->bounds.topLeft()) : std::nullopt;
}

std::optional<QPointF> DisplayManager::outputLocalToGlobal(const DisplayId &id,
                                                           const QPointF &point) const
{
    const auto *logical = display(id);
    return logical ? std::optional(point + logical->bounds.topLeft()) : std::nullopt;
}

const DisplaySnapshot *DisplayManager::physicalDisplay(const DisplayId &id) const
{
    if (!m_hardware)
        return nullptr;
    for (const DisplaySnapshot &physical : m_hardware->displays) {
        if (physical.sessionId == id)
            return &physical;
    }
    return nullptr;
}

} // namespace cutefish::display
