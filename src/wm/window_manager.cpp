// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/window_manager.h"
#include "scene/window_visual_node.h"
#include "wm/window_visual_controller.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QCoreApplication>
#include <QStringList>
#include <QTimer>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "core/logging.h"
#include "display/display_manager.h"
#include "input/hit_test.h"
#include "input/seat.h"
#include "input/session_gate.h"
#include "scene/node.h"
#include "scene/surface_node.h"
#include "scene/window_animation.h"
#include "wm/desks/desks_controller.h"
#include "wm/mru_window_tracker.h"
#include "wm/overview/overview_controller.h"
#include "wm/resize_cursor.h"
#include "wm/resize_handles.h"
#include "wm/shell_surface.h"
#include "wm/show_desktop_controller.h"
#include "wm/stacking.h"
#include "wm/toplevel_event_handler.h"
#include "wm/transient_manager.h"
#include "wm/window.h"
#include "wm/window_positioner.h"
#include "wm/window_state.h"
#include "wm/wm_event.h"

namespace cutefish::wm {

void WindowManager::setVisualStyle(const decoration::WindowVisualStyle &style)
{
    m_visualStyle = style;
    for (Window *window : windows()) {
        if (auto *node = dynamic_cast<scene::WindowVisualNode *>(window->visualNode()))
            node->setState(WindowVisualController::evaluate(*window, style));
    }
}

WindowManager::WindowManager(compositor::Compositor *compositor, input::Seat *seat, QObject *parent)
    : QObject(parent)
    , m_compositor(compositor)
    , m_seat(seat)
    , m_activation(this)
    , m_transientManager(std::make_unique<TransientManager>())
    , m_eventHandler(std::make_unique<ToplevelEventHandler>(this))
    , m_modality(this)
{
    m_activation.addObserver(this);
    m_mruTracker = std::make_unique<MruWindowTracker>(this);
    m_desks = std::make_unique<DesksController>(this);
    m_overview = std::make_unique<OverviewController>(this);
    m_showDesktop = std::make_unique<ShowDesktopController>(this);
}

WindowManager::~WindowManager()
{
    m_activation.removeObserver(this);
    m_showDesktop.reset();
    m_overview.reset();
    // Tear down through destroyWindow so activation and drags unwind cleanly.
    while (!m_stackingOrder.empty())
        destroyWindow(m_stackingOrder.back());
    m_desks.reset();
    m_mruTracker.reset();
}

void WindowManager::setOutputBounds(const QRect &bounds)
{
    if (bounds == m_outputBounds)
        return;
    m_outputBounds = bounds;
    // The reservations are edge thicknesses, so they survive a resolution
    // change and simply apply to the new output.
    setWorkArea(m_exclusiveZones.apply(m_outputBounds));
    // With a display manager its displaysChanged follows, once windows are
    // back on the new outputs.
    if (!m_displayManager)
        notifyOutputsChanged();
}

void WindowManager::setDisplayManager(display::DisplayManager *manager)
{
    if (m_displayManager == manager)
        return;
    if (m_displayManager)
        disconnect(m_displayManager, nullptr, this, nullptr);
    m_displayManager = manager;
    notifyOutputsChanged();
    if (!m_displayManager)
        return;
    connect(m_displayManager, &display::DisplayManager::displaysChanged, this,
            [this](uint64_t, uint64_t) {
                if (!m_displayManager->applied()
                    || m_displayManager->applied()->logicalDisplays.isEmpty())
                    return;
                const WMEvent event(WMEventType::WorkAreaChanged);
                for (Window *window : std::vector<Window *>(m_stackingOrder)) {
                    if (!hasWindow(window))
                        continue;
                    bool reachable = false;
                    if (m_displayManager->applied()) {
                        for (const auto &output : m_displayManager->applied()->logicalDisplays)
                            reachable |= window->bounds().intersects(output.bounds);
                    }
                    if (!reachable && m_displayManager->applied()
                        && !m_displayManager->applied()->logicalDisplays.isEmpty()
                        && window->stateType() == WindowStateType::Normal
                        && !window->isShellWindow()) {
                        const auto *output =
                                m_displayManager->nearestDisplay(window->bounds().center());
                        QRect target = window->bounds();
                        if (output) {
                            const QRect area = m_exclusiveZones.apply(
                                    output->workArea, output->waylandName, output->primary);
                            const int visibleWidth = std::min(64, target.width());
                            const int visibleHeight = std::min(32, target.height());
                            target.moveLeft(std::clamp(target.x(),
                                                       area.left() - target.width() + visibleWidth,
                                                       area.right() - visibleWidth + 1));
                            target.moveTop(std::clamp(target.y(), area.top(),
                                                      area.bottom() - visibleHeight + 1));
                            window->setBounds(target);
                        }
                    }
                    window->state()->onEvent(event);
                }
                notifyOutputsChanged();
            });
}

void WindowManager::notifyOutputsChanged()
{
    std::vector<OutputArea> areas = outputAreas();
    if (areas == m_outputAreas)
        return;
    m_outputAreas = std::move(areas);
    Q_EMIT outputsChanged();
}

QRect WindowManager::outputBounds() const
{
    if (m_displayManager && !m_displayManager->desktopBounds().isEmpty())
        return m_displayManager->desktopBounds();
    return m_outputBounds;
}

QRect WindowManager::outputBoundsFor(const Window *window) const
{
    if (m_displayManager) {
        const display::LogicalDisplay *logical = nullptr;
        if (window) {
            const auto target = m_windowOutputs.find(window);
            if (target != m_windowOutputs.end() && m_displayManager->applied()) {
                for (const auto &candidate : m_displayManager->applied()->logicalDisplays) {
                    if (candidate.waylandName == target->second) {
                        logical = &candidate;
                        break;
                    }
                }
            }
        }
        if (window && window->rootSurface() && m_displayManager->applied()) {
            const auto metadata = m_shellMetadata.find(window->rootSurface());
            if (metadata != m_shellMetadata.end() && !metadata->second.outputName.isEmpty()) {
                for (const auto &candidate : m_displayManager->applied()->logicalDisplays) {
                    if (candidate.waylandName == metadata->second.outputName) {
                        logical = &candidate;
                        break;
                    }
                }
            }
        }
        if (!logical)
            logical = window ? m_displayManager->displayForRect(window->bounds())
                             : m_displayManager->primaryDisplay();
        if (logical)
            return logical->bounds;
    }
    return m_outputBounds;
}

void WindowManager::setWindowOutput(Window *window, const QString &waylandName)
{
    if (window)
        m_windowOutputs[window] = waylandName;
}

void WindowManager::clearWindowOutput(Window *window)
{
    m_windowOutputs.erase(window);
}

QRect WindowManager::workAreaFor(const Window *window) const
{
    if (m_displayManager) {
        const display::LogicalDisplay *logical = window
                ? m_displayManager->displayForRect(window->bounds())
                : m_displayManager->primaryDisplay();
        if (logical)
            return window && window->isShellWindow()
                    ? logical->bounds
                    : m_exclusiveZones.apply(logical->workArea, logical->waylandName,
                                             logical->primary);
    }
    // A shell window places itself and is expected to cover the area it
    // reserved; confining it to the work area would move the dock off its own
    // strip.
    if (window && window->isShellWindow())
        return m_outputBounds;
    return m_workArea;
}

QRect WindowManager::visibleAreaFor(const Window *window) const
{
    QRect area = workAreaFor(window);
    QRect output = m_outputBounds;
    if (m_displayManager) {
        if (const auto *logical = window ? m_displayManager->displayForRect(window->bounds())
                                         : m_displayManager->primaryDisplay())
            output = logical->bounds;
    }
    if (!area.isEmpty() && output.bottom() > area.bottom())
        area.setBottom(output.bottom());
    return area;
}

QRect WindowManager::outputForNewWindow() const
{
    if (Window *active = m_activation.activeWindow();
        active && !active->isShellWindow() && !active->isPopup()) {
        return outputBoundsFor(active);
    }
    if (m_displayManager) {
        if (m_seat) {
            if (const auto *logical = m_displayManager->displayAt(m_seat->pointerLocation()))
                return logical->bounds;
        }
        if (const auto *logical = m_displayManager->primaryDisplay())
            return logical->bounds;
    }
    return m_outputBounds;
}

QRect WindowManager::workAreaForOutput(const QRect &outputBounds) const
{
    if (m_displayManager && m_displayManager->applied()) {
        for (const auto &logical : m_displayManager->applied()->logicalDisplays) {
            if (logical.bounds == outputBounds) {
                return m_exclusiveZones.apply(logical.workArea, logical.waylandName,
                                              logical.primary);
            }
        }
    }
    return outputBounds == m_outputBounds ? m_workArea : QRect();
}

std::vector<WindowManager::OutputArea> WindowManager::outputAreas() const
{
    std::vector<OutputArea> areas;
    if (m_displayManager && m_displayManager->applied()) {
        for (const auto &logical : m_displayManager->applied()->logicalDisplays) {
            if (!logical.bounds.isEmpty())
                areas.push_back({logical.bounds, workAreaForOutput(logical.bounds),
                                 logical.scale.value()});
        }
    }
    if (areas.empty() && !m_outputBounds.isEmpty()) {
        const double scale = m_compositor ? m_compositor->outputScale().value() : 1;
        areas.push_back({m_outputBounds, m_workArea.isEmpty() ? m_outputBounds : m_workArea,
                         scale});
    }
    return areas;
}

void WindowManager::setWorkArea(const QRect &bounds)
{
    const QRect area = bounds.intersected(m_outputBounds);
    if (area.isEmpty() || area == m_workArea)
        return;
    m_workArea = area;
    qCDebug(cfCompositor) << "work area" << m_workArea << "of output" << m_outputBounds;
    const WMEvent event(WMEventType::WorkAreaChanged);
    for (Window *window : std::vector<Window *>(m_stackingOrder)) {
        if (hasWindow(window))
            window->state()->onEvent(event);
    }
}

void WindowManager::applyExclusiveZones()
{
    setWorkArea(m_exclusiveZones.apply(m_outputBounds));
}

void WindowManager::setExclusiveZone(const void *token, ScreenEdge edge, int thickness,
                                     const QString &outputName)
{
    if (m_exclusiveZones.set(token, edge, thickness, outputName)) {
        applyExclusiveZones();
        if (!outputName.isEmpty()) {
            const WMEvent event(WMEventType::WorkAreaChanged);
            for (Window *window : std::vector<Window *>(m_stackingOrder)) {
                if (hasWindow(window))
                    window->state()->onEvent(event);
            }
        }
    }
}

void WindowManager::clearExclusiveZone(const void *token)
{
    if (m_exclusiveZones.clear(token)) {
        applyExclusiveZones();
        if (m_displayManager) {
            const WMEvent event(WMEventType::WorkAreaChanged);
            for (Window *window : std::vector<Window *>(m_stackingOrder)) {
                if (hasWindow(window))
                    window->state()->onEvent(event);
            }
        }
    }
}

void WindowManager::clearAllExclusiveZones()
{
    if (m_exclusiveZones.clearAll()) {
        applyExclusiveZones();
        if (m_displayManager) {
            const WMEvent event(WMEventType::WorkAreaChanged);
            for (Window *window : std::vector<Window *>(m_stackingOrder)) {
                if (hasWindow(window))
                    window->state()->onEvent(event);
            }
        }
    }
}

void WindowManager::setShellMetadata(compositor::Surface *surface,
                                     const ShellWindowMetadata &metadata)
{
    if (!surface)
        return;
    m_shellMetadata[surface] = metadata;

    // A surface that is already mapped picks the change up immediately; one
    // that is not gets it in createWindow(), before it is placed or announced.
    if (Window *window = windowForSurface(surface)) {
        if (window->rootSurface() == surface) {
            window->setRole(metadata.role);
            window->setFocusPolicy(metadata.focusPolicy);
            window->setPanelBehavior(metadata.panelBehavior);
            if (metadata.role == WindowRole::Sheet)
                attachSheet(window, metadata.parentWindow);
            else if (metadata.hasPosition)
                window->setClientPosition(metadata.position);
        }
    }
}

void WindowManager::clearShellMetadata(compositor::Surface *surface)
{
    m_shellMetadata.erase(surface);
}

const ShellWindowMetadata *WindowManager::shellMetadata(compositor::Surface *surface) const
{
    const auto it = m_shellMetadata.find(surface);
    return it == m_shellMetadata.end() ? nullptr : &it->second;
}

Window *WindowManager::createWindow(compositor::Surface *rootSurface, const WindowInit &init)
{
    auto owned = std::make_unique<Window>(this, rootSurface, init.title, init.type);
    Window *window = owned.get();
    m_windows.push_back(std::move(owned));

    window->setApplicationId(init.applicationId);
    window->setProcessId(init.processId);
    window->setMinimumSize(init.minimumSize);
    window->setMaximumSize(init.maximumSize);
    window->setFocusPolicy(window->isPopup() ? FocusPolicy::NoFocus : init.focusPolicy);
    window->setResizable(!window->isPopup());
    window->setCloseCallback(init.closeCallback);
    window->setTransientParent(init.transientParent);
    window->setModal(init.modal);
    if (window->isPopup()) {
        // The dock's tooltips and menus, including nested ones, appear at once.
        Window *root = window;
        while (hasWindow(root->transientParent()))
            root = root->transientParent();
        if (root->role() == WindowRole::Dock) {
            if (auto *animation = dynamic_cast<scene::WindowAnimation *>(window->node()))
                animation->setStyle(scene::WindowAnimation::Style::None);
        }
    }
    if (!window->isPopup() && m_systemDialogPolicy && init.processId > 0)
        window->setSystemDialog(m_systemDialogPolicy(init.processId));

    // The shell's metadata is applied before anything else looks at the window:
    // it decides the stacking layer this window is inserted into, whether it is
    // decorated, whether it is placed at all, and - because windowAdded is
    // emitted at the end of this function - whether the window-management
    // protocol ever announces it. Nothing here may be deferred to a signal
    // handler running after windowAdded.
    if (const ShellWindowMetadata *metadata = shellMetadata(rootSurface)) {
        window->setRole(metadata->role);
        window->setFocusPolicy(metadata->focusPolicy);
        window->setPanelBehavior(metadata->panelBehavior);
        if (metadata->role == WindowRole::Sheet)
            attachSheet(window, metadata->parentWindow);
    }

    m_stackingOrder.push_back(window);
    insertIntoStackingOrder(window);

    // Before the first placement, so the frame is part of what gets placed
    // rather than something that grows the window off the work area afterwards.
    // A shell window is never decorated.
    window->setDecorated(init.decorated && !window->isShellWindow() && !window->isPopup());

    QSize size = init.requestedSize;
    if (size.isEmpty() && rootSurface)
        size = rootSurface->contentSize();
    if (size.isEmpty())
        size = QSize(1, 1);

    const ShellWindowMetadata *metadata = shellMetadata(rootSurface);
    if (window->isPopup()) {
        window->setBounds(QRect(init.origin, size));
    } else if (window->role() == WindowRole::Sheet && window->transientParent()) {
        window->setBounds(QRect(QPoint(), size));
        placeSheet(window);
    } else if (metadata && metadata->hasPosition) {
        // Shell positions bypass the normal window placement policy.
        window->setBounds(QRect(metadata->position, size));
        window->setClientPosition(metadata->position);
    } else if (window->isShellWindow()) {
        window->setBounds(QRect(outputBoundsFor(window).topLeft(), size));
    } else if (Window *parent = window->transientParent()) {
        window->setBounds(m_placement.placeTransient(window->frameInsets().outset(size),
                                                     parent->bounds(), parent->frameInsets().top,
                                                     workAreaFor(parent)));
    } else {
        QRect workArea = workAreaForOutput(init.output);
        if (workArea.isEmpty())
            workArea = workAreaForOutput(outputForNewWindow());
        if (workArea.isEmpty())
            workArea = workAreaFor(window);
        window->setBounds(m_placement.place(window->frameInsets().outset(size), workArea));
        if (init.positionManaged)
            window->setBounds(boundsForNewWindow(window, window->bounds()));
    }

    // Onto a desk before anything is drawn or announced: a dialog of a window
    // on another desk starts out hidden with it.
    m_desks->onWindowAdded(window);

    restackScene();

    qCDebug(cfCompositor) << "Window created;" << toString(window->role()) << "total"
                          << m_stackingOrder.size();
    Q_EMIT windowAdded(window);
    return window;
}

QRect WindowManager::boundsForNewWindow(const Window *window, QRect bounds) const
{
    if (!restorePlacement(window, &bounds))
        bounds = window_positioner::boundsForNewWindow(window, bounds);
    return bounds;
}

void WindowManager::rememberPlacement(const Window *window)
{
    const compositor::Surface *surface = window->rootSurface();
    if (!surface || surface->restoreId().isEmpty() || window->applicationId().isEmpty()
        || window->isPopup() || window->isShellWindow() || window->transientParent()) {
        return;
    }
    // A maximized or fullscreen window is remembered by where it returns to.
    const WindowState *state = window->state();
    QRect bounds = window->bounds();
    if (!state->isNormal()) {
        if (!(state->isMaximized() || state->isFullscreen()) || !state->hasRestoreBounds())
            return;
        bounds = state->restoreBounds();
    }
    m_rememberedPlacements[{window->applicationId(), surface->restoreId()}] = {
            bounds, outputBoundsFor(window)};
}

bool WindowManager::restorePlacement(const Window *window, QRect *bounds) const
{
    const compositor::Surface *surface = window->rootSurface();
    if (!surface || surface->restoreId().isEmpty())
        return false;
    const auto it = m_rememberedPlacements.find({window->applicationId(), surface->restoreId()});
    if (it == m_rememberedPlacements.end())
        return false;
    const QRect workArea = workAreaForOutput(it->second.output);
    if (workArea.isEmpty())
        return false;
    const QRect &previous = it->second.bounds;
    bounds->moveTopLeft(previous.topLeft());
    if (surface->resizeAnchor() == compositor::ResizeAnchor::Center) {
        // Halves of the difference, as a client resize around the centre moves it.
        bounds->translate((previous.width() - bounds->width()) / 2,
                          (previous.height() - bounds->height()) / 2);
    }
    adjustBoundsToFit(workArea, bounds);
    return true;
}

void WindowManager::destroyWindow(Window *window)
{
    auto it = std::find_if(m_windows.begin(), m_windows.end(),
                           [window](const std::unique_ptr<Window> &candidate) {
                               return candidate.get() == window;
                           });
    if (it == m_windows.end())
        return;

    // Out of both lists before anyone is notified. The notifications below
    // re-enter this class - dropping activation schedules a repaint - and an
    // iterator into m_windows would not survive a window being added from
    // there. Ownership is held locally so the window stays valid meanwhile.
    std::unique_ptr<Window> owned = std::move(*it);
    // While the window still knows its output.
    rememberPlacement(window);
    m_windows.erase(it);
    m_windowOutputs.erase(window);
    m_stackingOrder.erase(std::remove(m_stackingOrder.begin(), m_stackingOrder.end(), window),
                          m_stackingOrder.end());

    // Drop auxiliary surface ownership before observers can look it up again.
    for (auto owner = m_surfaceOwners.begin(); owner != m_surfaceOwners.end();)
        owner = owner->second == window ? m_surfaceOwners.erase(owner) : std::next(owner);

    if (m_frameHover == window)
        m_frameHover = nullptr;
    std::erase_if(m_frameTouches, [window](const auto &entry) {
        return entry.second == window;
    });

    m_eventHandler->onWindowRemoving(window);
    m_desks->onWindowRemoving(window);
    m_transientManager->windowDestroying(window);
    const bool grabbed = m_keyboardGrab == window;
    if (grabbed)
        m_keyboardGrab = nullptr;
    // Activation moves on first, so the keyboard goes back to where it lands.
    m_activation.onWindowRemoving(window);
    if (grabbed)
        releaseKeyboardGrab();
    Q_EMIT pointerTargetsChanged();

    Q_EMIT windowRemoved(window);
}

void WindowManager::insertIntoStackingOrder(Window *window)
{
    auto it = std::find(m_stackingOrder.begin(), m_stackingOrder.end(), window);
    if (it == m_stackingOrder.end())
        return;

    const StackingLayer layer = layerFor(window);
    m_stackingOrder.erase(it);

    // Top of its own layer: the first window of a strictly higher layer is
    // where it goes.
    const auto target = std::find_if(m_stackingOrder.begin(), m_stackingOrder.end(),
                                     [layer](const Window *other) {
                                         return layerFor(other) > layer;
                                     });
    m_stackingOrder.insert(target, window);
}

void WindowManager::restackScene()
{
    // Keep transient families together, preserving sibling order within each layer.
    std::vector<Window *> ordered;
    std::function<void(Window *)> append = [&](Window *window) {
        ordered.push_back(window);
        for (Window *child : m_stackingOrder) {
            if (child->transientParent() == window)
                append(child);
        }
    };
    for (Window *window : m_stackingOrder) {
        if (!hasWindow(window->transientParent()))
            append(window);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Window *a, const Window *b) {
        return layerFor(a) < layerFor(b);
    });
    m_stackingOrder = std::move(ordered);

    for (Window *window : m_stackingOrder) {
        if (scene::Node *node = window->node())
            node->setExcludedFromCapture(layerFor(window) == StackingLayer::Capture);
    }

    scene::Node *container = m_compositor ? m_compositor->surfaceContainer() : nullptr;
    if (!container)
        return;

    if (cfCompositor().isDebugEnabled()) {
        QStringList order;
        for (Window *window : m_stackingOrder)
            order << QStringLiteral("%1(%2)").arg(window->title(), toString(window->role()));
        qCDebug(cfCompositor) << "stacking, bottom to top:" << order.join(QLatin1String(" < "));
    }

    std::vector<scene::Node *> managedNodes;
    auto appendAuxiliary = [&] {
        for (auto *surface : m_auxiliarySurfaces) {
            if (m_surfaceOwners.contains(surface))
                continue;
            if (auto *node = static_cast<scene::Node *>(surface->node());
                node && node->parent() == container)
                managedNodes.push_back(node);
        }
        for (scene::Node *overlay : m_deskOverlays) {
            if (overlay->parent() == container)
                managedNodes.push_back(overlay);
        }
    };
    bool appendedAuxiliary = false;
    for (Window *window : m_stackingOrder) {
        if (!appendedAuxiliary && layerFor(window) > StackingLayer::Normal) {
            appendAuxiliary();
            appendedAuxiliary = true;
        }
        if (window->state()->isMinimized())
            continue;
        if (m_dragOverlay && m_dragOverlayBelow == window && m_dragOverlay->parent() == container)
            managedNodes.push_back(m_dragOverlay);
        if (scene::Node *node = window->node(); node && node->parent() == container)
            managedNodes.push_back(node);

        for (const auto &[surface, owner] : m_surfaceOwners) {
            if (owner != window)
                continue;
            if (scene::Node *node = static_cast<scene::Node *>(surface->node());
                node && node->parent() == container) {
                managedNodes.push_back(node);
            }
        }
    }

    if (!appendedAuxiliary)
        appendAuxiliary();

    // Closing and minimized snapshots keep their slots while live windows reorder.
    auto order = container->children();
    size_t next = 0;
    for (scene::Node *&node : order) {
        if (std::find(managedNodes.begin(), managedNodes.end(), node) != managedNodes.end())
            node = managedNodes[next++];
    }

    // A dock transition passes over the panels, but not over the dock's own
    // popups: a tooltip must stay readable while a window flies in under it.
    // It goes straight above the topmost panel-layer toplevel.
    scene::Node *panelTop = nullptr;
    for (Window *window : m_stackingOrder) {
        if (window->isPopup() || window->state()->isMinimized()
            || layerFor(window) > StackingLayer::Dock)
            continue;
        if (scene::Node *node = window->node(); node && node->parent() == container)
            panelTop = node;
    }
    std::vector<scene::Node *> dockTransitions;
    for (scene::Node *node : order) {
        if (auto *animation = dynamic_cast<scene::WindowAnimation *>(node);
            m_dockTransitionsAbovePanels && animation && animation->drawsAbovePanels()
            && node != panelTop)
            dockTransitions.push_back(node);
    }
    if (!dockTransitions.empty()) {
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [&](scene::Node *node) {
                                       return std::find(dockTransitions.begin(),
                                                        dockTransitions.end(), node)
                                               != dockTransitions.end();
                                   }),
                    order.end());
        auto anchor = panelTop ? std::find(order.begin(), order.end(), panelTop) + 1 : order.end();
        order.insert(anchor, dockTransitions.begin(), dockTransitions.end());
    }

    container->reorderChildren(order);
    Q_EMIT pointerTargetsChanged();
}

void WindowManager::setDragOverlay(scene::Node *overlay, Window *below)
{
    if (m_dragOverlay == overlay && m_dragOverlayBelow == below)
        return;
    m_dragOverlay = overlay;
    m_dragOverlayBelow = overlay ? below : nullptr;
    restackScene();
}

void WindowManager::setDeskOverlays(std::vector<scene::Node *> overlays)
{
    m_deskOverlays = std::move(overlays);
    restackScene();
}

void WindowManager::setDockTransitionsAbovePanels(bool above)
{
    if (m_dockTransitionsAbovePanels == above)
        return;
    m_dockTransitionsAbovePanels = above;
    restackScene();
}

void WindowManager::stackAtBottomOfLayer(Window *window)
{
    auto it = std::find(m_stackingOrder.begin(), m_stackingOrder.end(), window);
    if (it == m_stackingOrder.end())
        return;
    const StackingLayer layer = layerFor(window);
    m_stackingOrder.erase(it);
    const auto target = std::find_if(m_stackingOrder.begin(), m_stackingOrder.end(),
                                     [layer](const Window *other) {
                                         return layerFor(other) >= layer;
                                     });
    m_stackingOrder.insert(target, window);
    restackScene();
    if (m_compositor)
        m_compositor->scheduleRepaint();
}

void WindowManager::raise(Window *window)
{
    if (!hasWindow(window))
        return;

    for (Window *ancestor = window; hasWindow(ancestor); ancestor = ancestor->transientParent())
        insertIntoStackingOrder(ancestor);
    restackScene();

    if (m_compositor)
        m_compositor->scheduleRepaint();
}

void WindowManager::onWindowStackingChanged(Window *window)
{
    if (!hasWindow(window))
        return;
    // Its layer may have changed, and with it whether it is on a desk.
    m_desks->updateWindow(window);
    insertIntoStackingOrder(window);
    restackScene();
    if (m_compositor)
        m_compositor->scheduleRepaint();
}

void WindowManager::attachSheet(Window *sheet, quint64 parentId)
{
    Window *parent = windowById(parentId);
    // Only an application window has a sheet; another shell surface is no
    // place to hang one from.
    if (parent == sheet || (parent && (parent->isShellWindow() || parent->isPopup())))
        parent = nullptr;
    sheet->setTransientParent(parent);
    sheet->setModal(parent != nullptr);
    if (hasWindow(sheet))
        placeSheet(sheet);
}

void WindowManager::placeSheet(Window *sheet)
{
    Window *parent = sheet->transientParent();
    if (!hasWindow(sheet) || !hasWindow(parent))
        return;
    // What the parent shows: a client that has not drawn a new size yet, as a
    // hung one never does, keeps its old contents where they were.
    const QRect bounds = parent->presentedBounds().translated(parent->bounds().topLeft());
    const QSize size = sheet->bounds().size();
    sheet->setClientPosition(QPoint(bounds.x() + (bounds.width() - size.width()) / 2,
                                    bounds.y() + (bounds.height() - size.height()) / 2));
}

void WindowManager::onWindowModalChanged(Window *window)
{
    if (hasWindow(window))
        m_modality.updateModality(window, true);
}

void WindowManager::onWindowModalityInputsChanged(Window *window)
{
    if (hasWindow(window))
        m_modality.updateModality(window, false);
}

void WindowManager::onWindowVisibilityChanged(Window *window)
{
    if (!window->isVisible() && m_keyboardGrab == window)
        releaseKeyboardGrab();
    Q_EMIT pointerTargetsChanged();
}

void WindowManager::onWindowTargetBoundsChanged(Window *window, const QRect &oldBounds,
                                                const QRect &newBounds)
{
    const scene::Node *node = window->node();
    if (!m_seat || !hasWindow(window) || !node || !node->isEffectivelyVisible()
        || node->isEventTargetingBlocked())
        return;
    // As aura's WindowEventDispatcher::OnWindowBoundsChanged: only when the
    // pointer enters or leaves the bounds, or the bounds move under it. A
    // resize in place leaves the surface-local location as it was.
    const QPointF cursor = m_seat->cursorPosition();
    const QPoint location(static_cast<int>(std::floor(cursor.x())),
                          static_cast<int>(std::floor(cursor.y())));
    const bool contained = newBounds.contains(location);
    if (oldBounds.contains(location) != contained
        || (contained && newBounds.topLeft() != oldBounds.topLeft()))
        Q_EMIT pointerTargetsChanged();
}

void WindowManager::setKeyboardGrab(Window *window)
{
    if (!hasWindow(window) || !window->isVisible() || !allowsActivation())
        return;
    m_keyboardGrab = window;
    // Borrowed: a keyboard leave would make Qt draw the application inactive.
    if (m_seat)
        m_seat->setFocusedSurface(window->rootSurface(), true);
}

void WindowManager::activateForPress(Window *window)
{
    // A press through the capture interface, as while it counts down, takes
    // the keyboard back even when the window pressed is already active.
    Window *root = window;
    while (root && root->transientParent())
        root = root->transientParent();
    const bool released = m_keyboardGrab && root != m_keyboardGrab;
    if (released)
        m_keyboardGrab = nullptr;
    m_activation.activate(window);
    if (released)
        releaseKeyboardGrab();
}

void WindowManager::releaseKeyboardGrab()
{
    m_keyboardGrab = nullptr;
    Window *active = m_activation.activeWindow();
    if (m_seat)
        m_seat->setFocusedSurface(active ? active->rootSurface() : nullptr);
}

Window *WindowManager::windowById(quint64 id) const
{
    for (Window *window : m_stackingOrder) {
        if (window->id() == id)
            return window;
    }
    return nullptr;
}

bool WindowManager::belongsToApplication(const Window *candidate, const Window *window) const
{
    if (candidate->skipTaskbar())
        return false;
    if (candidate == window)
        return true;
    // The application id first, as the dock groups its tasks: an installed web
    // app shares its browser's process but is an application of its own.
    if (!window->applicationId().isEmpty()) {
        return candidate->applicationId().compare(window->applicationId(), Qt::CaseInsensitive)
                == 0;
    }
    // With neither an application id nor a process, a window is on its own.
    return window->processId() != 0 && candidate->applicationId().isEmpty()
            && candidate->processId() == window->processId();
}

bool WindowManager::hasApplicationWindow(qint64 process) const
{
    return std::any_of(m_stackingOrder.begin(), m_stackingOrder.end(), [process](Window *window) {
        return !window->skipTaskbar() && window->processId() == process;
    });
}

void WindowManager::quitApplication(Window *window)
{
    while (window && window->transientParent())
        window = window->transientParent();
    if (!window || window->skipTaskbar())
        return;

    std::vector<Window *> windows;
    std::set<qint64> processes;
    for (Window *candidate : m_stackingOrder) {
        if (candidate->transientParent() || !belongsToApplication(candidate, window))
            continue;
        windows.push_back(candidate);
        // Never the compositor itself, whatever claims its pid.
        if (candidate->processId() != 0
            && candidate->processId() != QCoreApplication::applicationPid())
            processes.insert(candidate->processId());
    }
    // A close request can destroy its window, and with it others.
    for (Window *target : windows) {
        if (std::find(m_stackingOrder.begin(), m_stackingOrder.end(), target)
                    != m_stackingOrder.end()
            && target->canClose())
            target->requestClose();
    }

    // A process left with no window at all is terminated: the application
    // closed its windows but keeps running. One that still shows a window
    // declined, typically to ask about unsaved changes, or also runs another
    // application's windows, and is left alone.
    QTimer::singleShot(m_quitGracePeriod, this, [this, processes] {
        if (!m_terminateProcess)
            return;
        for (const qint64 process : processes) {
            if (!hasApplicationWindow(process))
                m_terminateProcess(process);
        }
    });
}

void WindowManager::forceQuit(Window *window)
{
    if (!hasWindow(window))
        return;
    const qint64 process = window->processId();
    window->kill();
    // Never the compositor itself, whatever claims its pid.
    if (m_forceQuitProcess && process != 0 && process != QCoreApplication::applicationPid())
        m_forceQuitProcess(process);
}

bool WindowManager::promptNotResponding(Window *window)
{
    return m_notRespondingPrompt && hasWindow(window) && m_notRespondingPrompt(window);
}

void WindowManager::hideApplication(Window *window)
{
    if (!window || window->skipTaskbar())
        return;
    const std::vector<Window *> windows = m_stackingOrder;
    for (Window *candidate : windows) {
        if (belongsToApplication(candidate, window))
            candidate->setHidden(true);
    }
}

void WindowManager::unhideApplication(Window *window)
{
    if (!window || window->skipTaskbar())
        return;
    const std::vector<Window *> windows = m_stackingOrder;
    for (Window *candidate : windows) {
        if (candidate->isHidden() && belongsToApplication(candidate, window))
            candidate->setHidden(false);
    }
}

void WindowManager::hideOtherApplications(Window *window)
{
    const bool keep = window && !window->skipTaskbar();
    const std::vector<Window *> windows = m_stackingOrder;
    for (Window *candidate : windows) {
        if (candidate->skipTaskbar() || (keep && belongsToApplication(candidate, window)))
            continue;
        candidate->setHidden(true);
    }
}

Window *WindowManager::windowForSurface(compositor::Surface *surface) const
{
    while (surface && surface->parentSurface())
        surface = surface->parentSurface();
    if (!surface)
        return nullptr;

    for (Window *window : m_stackingOrder) {
        if (window->rootSurface() == surface)
            return window;
    }

    const auto owner = m_surfaceOwners.find(surface);
    return owner != m_surfaceOwners.end() ? owner->second : nullptr;
}

void WindowManager::setSurfaceOwner(compositor::Surface *surface, Window *owner)
{
    if (!surface)
        return;
    if (owner)
        m_surfaceOwners[surface] = owner;
    else
        m_surfaceOwners.erase(surface);
}

void WindowManager::clearSurfaceOwner(compositor::Surface *surface)
{
    m_surfaceOwners.erase(surface);
    std::erase(m_auxiliarySurfaces, surface);
    restackScene();
}

void WindowManager::setAuxiliarySurface(compositor::Surface *surface, Window *owner)
{
    if (!surface)
        return;
    const bool added = std::find(m_auxiliarySurfaces.begin(), m_auxiliarySurfaces.end(), surface)
            == m_auxiliarySurfaces.end();
    if (added)
        m_auxiliarySurfaces.push_back(surface);
    const auto current = m_surfaceOwners.find(surface);
    // An X11 override-redirect window is presented again on every commit.
    if (!added && (current == m_surfaceOwners.end() ? nullptr : current->second) == owner)
        return;
    setSurfaceOwner(surface, owner);
    restackScene();
}

void WindowManager::pushPopupGrab(ShellSurface *popup)
{
    if (!popup
        || std::find(m_popupGrabs.begin(), m_popupGrabs.end(), popup) != m_popupGrabs.end()) {
        return;
    }
    m_popupGrabs.push_back(popup);
}

void WindowManager::removePopupGrab(ShellSurface *popup)
{
    m_popupGrabs.erase(std::remove(m_popupGrabs.begin(), m_popupGrabs.end(), popup),
                       m_popupGrabs.end());
}

bool WindowManager::isInsidePopupGrabChain(compositor::Surface *surface) const
{
    while (surface && surface->parentSurface())
        surface = surface->parentSurface();
    if (!surface)
        return false;
    // A popup opened from a grabbing one without a grab of its own is still inside.
    for (ShellSurface *popup : m_popupGrabs) {
        if (popup->rootSurface() == surface || popup->hasPopupDescendant(surface))
            return true;
    }
    return false;
}

bool WindowManager::isCaptureWindow(const Window *window) const
{
    // Capture mode leaves open menus up so they can be in the picture; a press
    // on the capture interface is the capture's, not the end of the grab.
    return window && layerFor(window) == StackingLayer::Capture;
}

void WindowManager::dismissPopupGrabs()
{
    // Innermost first, one at a time, each taken out of the list before its
    // client is told. Dismissing one popup routinely destroys the rest of the
    // chain, which unregisters them from here; a cached copy of the list would
    // be full of dangling pointers by the second iteration.
    qCDebug(cfCompositor) << "dismissing" << m_popupGrabs.size() << "grabbing popup(s)";
    while (!m_popupGrabs.empty()) {
        ShellSurface *innermost = m_popupGrabs.back();
        m_popupGrabs.pop_back();
        innermost->close();
    }
}

void WindowManager::cancelInteractions()
{
    m_eventHandler->completeDrag(ToplevelEventHandler::DragResult::Success);
    for (Window *window : m_stackingOrder) {
        if (WindowFrame *frame = window->frame())
            frame->cancelPress();
    }
}

bool WindowManager::allowsActivation() const
{
    return !m_sessionGate || m_sessionGate->allowsActivation();
}

scene::Node *WindowManager::inputRoot() const
{
    return input::inputRootFor(m_compositor, m_sessionGate);
}

Window *WindowManager::windowAt(const QPointF &location, compositor::Surface **hitSurface,
                                QPointF *surfaceLocation) const
{
    QPointF localLocation;
    compositor::Surface *target =
            input::findTargetSurfaceIn(m_compositor, inputRoot(), location, &localLocation);
    if (hitSurface)
        *hitSurface = target;
    if (surfaceLocation)
        *surfaceLocation = localLocation;
    return windowForSurface(target);
}

Window *WindowManager::capturableWindowAt(const QPointF &location) const
{
    Window *window = windowForSurface(input::findCapturedSurfaceIn(
            m_compositor, m_compositor ? m_compositor->surfaceContainer() : nullptr, location));
    while (window && window->isPopup() && window->transientParent())
        window = window->transientParent();
    return window;
}

Window *WindowManager::modalBlockedWindowAt(const QPointF &location) const
{
    // Hit testing on every motion only pays off while some modal is up.
    const bool anyModal =
            std::any_of(m_stackingOrder.begin(), m_stackingOrder.end(), [](const Window *window) {
                return isModalInEffect(window);
            });
    if (!anyModal)
        return nullptr;
    Window *target = frameAt(location);
    if (!target)
        target = windowAt(location);
    return modalTransient(target) ? target : nullptr;
}

bool WindowManager::isPopupSurface(compositor::Surface *surface) const
{
    while (surface && surface->parentSurface())
        surface = surface->parentSurface();
    Window *window = windowForSurface(surface);
    return (window && window->isPopup())
            || (surface && m_surfaceOwners.find(surface) != m_surfaceOwners.end())
            || input::isInputMethodSurface(m_compositor, surface);
}

bool WindowManager::isInputMethodPress(const QPointF &location) const
{
    compositor::Surface *hit = nullptr;
    windowAt(location, &hit, nullptr);
    return input::isInputMethodSurface(m_compositor, hit);
}

Window *WindowManager::frameAt(const QPointF &location, FrameArea *area) const
{
    if (area)
        *area = FrameArea::None;

    // Server-side frames belong to the session, so a blocked one owns none of
    // the screen: no move, no resize, no caption button while locked.
    if (!allowsActivation())
        return nullptr;

    // A popup covering a frame receives client input instead of frame actions.
    if (isPopupAt(location))
        return nullptr;

    for (auto it = m_stackingOrder.rbegin(); it != m_stackingOrder.rend(); ++it) {
        Window *window = *it;
        if (!window->isVisible())
            continue;

        WindowFrame *frame = window->frame();
        if (frame && frame->inputBounds().contains(location.toPoint())) {
            const FrameArea hitArea = frame->areaAt(location);
            if (hitArea != FrameArea::None) {
                if (area)
                    *area = hitArea;
                return window;
            }
            // Inside this window, but in the part the client owns.
            return nullptr;
        }
        if (window->bounds().contains(location.toPoint())
            && (!window->visualNode() || window->visualNode()->acceptsInput(location)))
            return nullptr;
    }
    return nullptr;
}

bool WindowManager::isPopupAt(const QPointF &location) const
{
    QPointF surfaceLocation;
    return isPopupSurface(m_seat ? m_seat->pointerTargetAt(inputRoot(), location, &surfaceLocation)
                                 : input::findTargetSurfaceIn(m_compositor, inputRoot(), location,
                                                              &surfaceLocation));
}

Window *WindowManager::resizeHandleAt(const QPointF &location, bool touch, ResizeEdge *edge) const
{
    *edge = ResizeEdge::None;
    if (!allowsActivation() || isPopupAt(location))
        return nullptr;

    for (auto it = m_stackingOrder.rbegin(); it != m_stackingOrder.rend(); ++it) {
        Window *window = *it;
        if (!window->isVisible())
            continue;

        // Checked before the window's own area: the handles reach a pixel in.
        const ResizeEdge hit = serverResizeEdgeAt(*window, m_visualStyle, location, touch);
        if (hit != ResizeEdge::None) {
            *edge = hit;
            return window;
        }
        if (WindowFrame *frame = window->frame();
            frame && frame->inputBounds().contains(location.toPoint()))
            return nullptr;
        if (window->bounds().contains(location.toPoint())
            && (!window->visualNode() || window->visualNode()->acceptsInput(location)))
            return nullptr;
    }
    return nullptr;
}

bool WindowManager::hasWindow(const Window *window) const
{
    return std::find(m_stackingOrder.begin(), m_stackingOrder.end(), window)
            != m_stackingOrder.end();
}

Window *WindowManager::windowTrackingFramePress() const
{
    for (Window *window : m_stackingOrder) {
        WindowFrame *frame = window->frame();
        if (frame && frame->isTrackingPress())
            return window;
    }
    return nullptr;
}

void WindowManager::setFrameHover(Window *window)
{
    if (m_frameHover == window)
        return;
    if (m_frameHover && m_frameHover->frame())
        m_frameHover->frame()->onPointerLeave();
    m_frameHover = window;
}

bool WindowManager::onPointerMotion(input::Timestamp timestamp, const QPointF &location)
{
    Q_EMIT pointerMoved(location);
    if (m_eventHandler->onPointerMotion(location))
        return true;

    // A held caption button keeps the pointer until it is released, exactly as
    // a pressed button does inside a client.
    if (Window *pressed = windowTrackingFramePress()) {
        setFrameHover(pressed);
        return pressed->frame()->onPointerMotion(timestamp, location);
    }

    // A client holding the implicit grab owns the pointer until it releases:
    // a frame the pointer happens to travel over must not take the motion, nor
    // change the cursor shape out from under the drag.
    if (m_seat && m_seat->pointerGrabSurface())
        return false;

    // A window blocked by a modal gets no pointer input, not even hover. As in
    // exo, pointer focus and the cursor stay with whatever had them.
    if (Window *blocked = modalBlockedWindowAt(location)) {
        m_modality.processLocatedEvent(blocked, false);
        setFrameHover(nullptr);
        return true;
    }

    ResizeEdge edge = ResizeEdge::None;
    if (resizeHandleAt(location, false, &edge)) {
        // Compositor-owned pixels, as a frame's are: the client loses the pointer.
        setFrameHover(nullptr);
        if (m_seat) {
            m_seat->setCompositorCursorShape(resizeCursor(edge));
            m_seat->clearPointerFocus();
        }
        return true;
    }

    Window *window = frameAt(location);
    if (m_seat)
        m_seat->setCompositorCursorShape(window ? window->frame()->cursorShapeAt(location)
                                                : input::CursorShape::Default);
    setFrameHover(window);
    if (!window)
        return false;

    // The pointer is over compositor-owned pixels: whatever client had it must
    // be told it left, and must not see this motion.
    if (m_seat)
        m_seat->clearPointerFocus();
    window->frame()->onPointerMotion(timestamp, location);
    return true;
}

bool WindowManager::onPointerButton(input::Timestamp timestamp, const QPointF &location,
                                    uint32_t code, bool pressed)
{
    if (pressed) {
        // A press the session gate took never reached here to be released.
        m_consumedButtons.erase(code);
        const bool consumed = handlePointerButton(timestamp, location, code, true);
        if (consumed)
            m_consumedButtons.insert(code);
        return consumed;
    }
    // Drags and caption buttons still need to see the release they end on.
    const bool consumed = handlePointerButton(timestamp, location, code, false);
    return m_consumedButtons.erase(code) > 0 || consumed;
}

bool WindowManager::handlePointerButton(input::Timestamp timestamp, const QPointF &location,
                                        uint32_t code, bool pressed)
{
    qCDebug(cfInputTransitions) << "WM code" << code << (pressed ? "press" : "release")
                                << "drag-target" << m_eventHandler->draggedWindow()
                                << "frame-capture" << windowTrackingFramePress() << "hit-frame"
                                << frameAt(location);
    if (m_eventHandler->isDragging())
        return pressed ? true : m_eventHandler->onPointerButtonReleased();

    if (Window *tracking = windowTrackingFramePress())
        return tracking->frame()->onPointerButton(timestamp, location, pressed);

    if (!pressed)
        return false;

    // Further buttons pressed during a client's implicit grab belong to it.
    if (m_seat && m_seat->pointerGrabSurface())
        return false;

    if (isInputMethodPress(location))
        return false;

    if (!m_popupGrabs.empty()) {
        compositor::Surface *hit = nullptr;
        Window *target = windowAt(location, &hit, nullptr);
        if (!isInsidePopupGrabChain(hit) && !isCaptureWindow(target)) {
            // The press belongs to the grab, not to whatever is underneath.
            dismissPopupGrabs();
            return true;
        }
    }

    if (Window *blocked = modalBlockedWindowAt(location)) {
        m_modality.processLocatedEvent(blocked, true);
        return true;
    }

    // A press on a window moved aside brings them all back, that one in
    // front, and goes no further.
    if (m_showDesktop->isActive()) {
        Window *window = frameAt(location);
        if (!window)
            window = windowAt(location);
        if (window && m_showDesktop->isMovedAside(window)) {
            m_showDesktop->exit(window);
            return true;
        }
    }

    ResizeEdge edge = ResizeEdge::None;
    if (Window *window = resizeHandleAt(location, false, &edge)) {
        activateForPress(window);
        if (!hasWindow(window))
            return true;
        window->ping();
        m_eventHandler->beginResize(window, edge, location, std::nullopt);
        return true;
    }

    if (Window *window = frameAt(location)) {
        // Activation first: a press on a frame raises its window, whatever the
        // press then does.
        activateForPress(window);
        // Activation runs client-visible code, which can take the window with
        // it; the frame is only touched again once the window is still here.
        if (!hasWindow(window))
            return true;
        window->ping();
        if (m_seat)
            m_seat->clearPointerFocus();
        setFrameHover(window);
        window->frame()->onPointerButton(timestamp, location, true);
        return true;
    }

    // Click to activate. The hit surface may be a subsurface; activation always
    // lands on the window that owns the root of the hierarchy.
    Window *window = windowAt(location);
    activateForPress(window);
    // A press is where a stalled client gets noticed, so each one pings it.
    if (window && hasWindow(window))
        window->ping();
    // The press itself still belongs to the client.
    return false;
}

bool WindowManager::onTouchDown(input::Timestamp timestamp, int32_t id, const QPointF &location)
{
    // A window drag, pointer or touch, owns the seat until it ends.
    if (m_eventHandler->isDragging())
        return true;

    if (isInputMethodPress(location))
        return false;

    if (!m_popupGrabs.empty()) {
        compositor::Surface *hit = nullptr;
        Window *target = windowAt(location, &hit, nullptr);
        if (!isInsidePopupGrabChain(hit) && !isCaptureWindow(target)) {
            dismissPopupGrabs();
            return true;
        }
    }

    if (Window *blocked = modalBlockedWindowAt(location)) {
        m_modality.processLocatedEvent(blocked, true);
        return true;
    }

    ResizeEdge edge = ResizeEdge::None;
    if (Window *window = resizeHandleAt(location, true, &edge)) {
        activateForPress(window);
        if (!hasWindow(window))
            return true;
        window->ping();
        m_eventHandler->beginResize(window, edge, location, id);
        return true;
    }

    // A touch activates the window it lands on, as wm::FocusController does
    // on a touch press, frame or client area alike.
    if (Window *window = frameAt(location)) {
        activateForPress(window);
        if (!hasWindow(window))
            return true;
        window->ping();
        if (WindowFrame *frame = window->frame()) {
            m_frameTouches[id] = window;
            frame->onTouchDown(timestamp, id, location);
        }
        return true;
    }
    Window *window = windowAt(location);
    activateForPress(window);
    if (window && hasWindow(window))
        window->ping();
    return false;
}

bool WindowManager::onTouchMotion(input::Timestamp timestamp, int32_t id,
                                  const QPointF &location)
{
    // Checked before the frame: a point the frame started a drag with moves
    // the window from then on.
    if (m_eventHandler->onTouchMotion(id, location))
        return true;
    const auto it = m_frameTouches.find(id);
    if (it == m_frameTouches.end())
        return false;
    if (WindowFrame *frame = it->second->frame())
        frame->onTouchMotion(timestamp, id, location);
    return true;
}

bool WindowManager::onTouchUp(input::Timestamp timestamp, int32_t id)
{
    const auto it = m_frameTouches.find(id);
    Window *frameWindow = it != m_frameTouches.end() ? it->second : nullptr;
    if (frameWindow)
        m_frameTouches.erase(it);
    if (m_eventHandler->onTouchUp(id))
        return true;
    if (!frameWindow)
        return false;
    if (WindowFrame *frame = frameWindow->frame())
        frame->onTouchUp(timestamp, id);
    return true;
}

void WindowManager::onTouchCancel(input::Timestamp)
{
    m_eventHandler->onTouchCancel();
    for (const auto &[id, window] : std::exchange(m_frameTouches, {})) {
        if (hasWindow(window) && window->frame())
            window->frame()->onTouchCancel();
    }
}

bool WindowManager::onPointerAxis(input::Timestamp, const QPointF &location, const QPointF &, bool)
{
    if (m_eventHandler->isDragging())
        return true;
    if (m_seat && m_seat->pointerGrabSurface())
        return false;
    return modalBlockedWindowAt(location) != nullptr;
}

bool WindowManager::onPointerAxisStop(input::Timestamp, const QPointF &location)
{
    if (m_eventHandler->isDragging())
        return true;
    if (m_seat && m_seat->pointerGrabSurface())
        return false;
    return modalBlockedWindowAt(location) != nullptr;
}

bool WindowManager::onKey(input::Timestamp, uint32_t evdevCode, bool pressed)
{
    // A blocked session has no interactive drag to feed and no shortcut of its
    // own; the key belongs to the lock screen.
    if (!allowsActivation())
        return false;
    // Typing is waiting on the window as much as a press is.
    if (pressed && !m_keyboardGrab) {
        if (Window *active = m_activation.activeWindow())
            active->ping();
    }
    if (!m_eventHandler->isDragging())
        return false;
    if (pressed)
        m_eventHandler->onKeyPressed(evdevCode);
    // The whole key stream belongs to the drag: a client must never see a
    // release whose press it did not get.
    return true;
}

void WindowManager::onWindowActivated(Window *gainedActive, Window *lostActive)
{
    Q_UNUSED(lostActive);
    if (m_seat && !m_keyboardGrab)
        m_seat->setFocusedSurface(gainedActive ? gainedActive->rootSurface() : nullptr);
    Q_EMIT activeWindowChanged(gainedActive);
}

} // namespace cutefish::wm
