// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/snap_drag_controller.h"

#include <QAbstractEventDispatcher>
#include <QLineF>

#include "compositor/compositor.h"
#include "core/logging.h"
#include "scene/snap_preview_node.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

namespace cutefish::wm {

namespace {

// How long the pointer has to stay against the top edge before the drag turns
// into a maximize. Upstream: kDwellTime.
constexpr int kDwellMs = 400;

// Moving this far restarts the countdown, so a window dragged across the top
// of the screen does not maximize on the way past. Upstream:
// kSnapDragDwellTimeResetThreshold.
constexpr int kDwellResetThreshold = 8;

} // namespace

SnapDragController::SnapDragController(WindowManager *manager)
    : m_manager(manager)
{
    m_dwellTimer.setSingleShot(true);
    m_dwellTimer.setInterval(kDwellMs);
    QObject::connect(&m_dwellTimer, &QTimer::timeout, [this] {
        fireDwell();
    });
}

SnapDragController::~SnapDragController()
{
    clearPreview();
}

void SnapDragController::begin(Window *window, const QPointF &start)
{
    m_window = window;
    m_snapType = SnapType::None;
    m_canSnapToMaximize = window && isBelowTopEdge(start);
    m_dwellLocation.reset();
    stopDwell();
}

bool SnapDragController::isBelowTopEdge(const QPointF &location) const
{
    const QRect area =
            nonSnapArea(m_manager->workAreaFor(m_window), m_manager->outputBoundsFor(m_window));
    return !area.isEmpty() && location.y() > area.top();
}

void SnapDragController::startDwell()
{
    m_dwellPending = true;
    // Tests drive the countdown by hand and have no event loop to run a timer
    // in; starting one there only warns.
    if (QAbstractEventDispatcher::instance())
        m_dwellTimer.start();
}

void SnapDragController::stopDwell()
{
    m_dwellPending = false;
    m_dwellTimer.stop();
}

void SnapDragController::fireDwell()
{
    if (!m_dwellPending)
        return;
    stopDwell();
    setSnapType(constrained(SnapType::Maximize));
}

SnapType SnapDragController::constrained(SnapType type) const
{
    if (!m_window)
        return SnapType::None;
    switch (type) {
    case SnapType::Primary:
    case SnapType::Secondary:
        return m_window->state()->canSnap() ? type : SnapType::None;
    case SnapType::Maximize:
        return m_window->isResizable() ? type : SnapType::None;
    case SnapType::None:
        break;
    }
    return SnapType::None;
}

void SnapDragController::update(const QPointF &location)
{
    if (!m_window)
        return;

    if (!m_canSnapToMaximize)
        m_canSnapToMaximize = isBelowTopEdge(location);

    const SnapType type = constrained(snapTypeAt(location, m_manager->workAreaFor(m_window),
                                                 m_manager->outputBoundsFor(m_window)));

    // Only the top edge dwells. The sides snap as soon as the pointer reaches
    // them, which is what makes a deliberate throw to the side feel immediate.
    if (type == SnapType::Maximize) {
        if (m_canSnapToMaximize) {
            const bool movedAway = m_dwellLocation
                    && QLineF(*m_dwellLocation, location).length() > kDwellResetThreshold;
            if (!m_dwellPending || movedAway) {
                startDwell();
                if (movedAway)
                    setSnapType(SnapType::None);
            }
        }
        m_dwellLocation = location;
        return;
    }

    stopDwell();
    m_dwellLocation.reset();
    setSnapType(type);
}

SnapType SnapDragController::end()
{
    const SnapType type = m_snapType;
    stopDwell();
    m_dwellLocation.reset();
    clearPreview();
    m_window = nullptr;
    m_snapType = SnapType::None;
    return type;
}

void SnapDragController::setSnapType(SnapType type)
{
    if (m_snapType == type)
        return;
    m_snapType = type;
    qCDebug(cfCompositor) << "drag snap target:" << toString(type);
    updatePreview();
}

void SnapDragController::updatePreview()
{
    if (m_snapType == SnapType::None || !m_window) {
        clearPreview();
        return;
    }

    const QRect bounds = snapBounds(m_snapType, m_manager->workAreaFor(m_window));
    scene::Node *container =
            m_manager->compositor() ? m_manager->compositor()->surfaceContainer() : nullptr;
    if (bounds.isEmpty() || !container)
        return;

    if (!m_preview) {
        m_preview = static_cast<scene::SnapPreviewNode *>(
                container->addChild(std::make_unique<scene::SnapPreviewNode>()));
        const auto &style = m_manager->visualStyle();
        m_preview->setColors(style.snapPreviewFill, style.snapPreviewOutline,
                             style.snapPreviewHighlight);
        m_manager->setDragOverlay(m_preview, m_window);
    }
    m_preview->show(bounds);
    if (m_manager->compositor())
        m_manager->compositor()->scheduleRepaint();
}

void SnapDragController::clearPreview()
{
    if (!m_preview)
        return;
    m_manager->setDragOverlay(nullptr, nullptr);
    if (scene::Node *parent = m_preview->parent())
        parent->removeChild(m_preview);
    m_preview = nullptr;
    if (m_manager->compositor())
        m_manager->compositor()->scheduleRepaint();
}

} // namespace cutefish::wm
