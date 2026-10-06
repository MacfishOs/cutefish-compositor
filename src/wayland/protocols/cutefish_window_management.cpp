// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/cutefish_window_management.h"

#include <cutefish-window-management-v1-server-protocol.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include <QImage>

#include "compositor/surface.h"
#include "core/logging.h"
#include "input/seat.h"
#include "wayland/privileged_client.h"
#include "wayland/server_util.h"
#include "wm/activation.h"
#include "wm/desks/desk.h"
#include "wm/desks/desks_controller.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

namespace cutefish::wayland {

namespace {

// The user data of a cutefish_window_v1. It stores the window's identifier
// rather than its address: every request here runs client-visible code that may
// destroy the window, and an identifier can always be revalidated against the
// window manager while a stale pointer cannot.
struct WindowResourceData {
    WaylandCutefishWindowManagement* management = nullptr;
    quint64 windowId = 0;
};

wm::Window* resolve(wl_resource* resource)
{
    auto* data = getUserDataAs<WindowResourceData>(resource);
    if (!data || data->windowId == 0)
        return nullptr;
    return data->management->windowManager()->windowById(data->windowId);
}

// A sealed memfd holding `image` as wl_shm argb8888 rows, or -1.
int thumbnailFile(const QImage& image)
{
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const size_t row = size_t(pixels.width()) * 4;
    const size_t size = row * pixels.height();
    int fd = memfd_create("cutefish-window-thumbnail", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, off_t(size)) < 0) {
        close(fd);
        return -1;
    }
    for (int y = 0; y < pixels.height(); ++y) {
        const auto* data = reinterpret_cast<const char*>(pixels.constScanLine(y));
        size_t written = 0;
        while (written < row) {
            const ssize_t result =
                pwrite(fd, data + written, row - written, off_t(row * y + written));
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0) {
                close(fd);
                return -1;
            }
            written += size_t(result);
        }
    }
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    return fd;
}

uint32_t idHigh(quint64 id)
{
    return static_cast<uint32_t>(id >> 32);
}

uint32_t idLow(quint64 id)
{
    return static_cast<uint32_t>(id & 0xffffffffu);
}

void windowDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void windowActivate(wl_client*, wl_resource* resource)
{
    wm::Window* window = resolve(resource);
    if (!window)
        return;
    // A hidden or minimized window has to come back before it can hold the
    // focus; this is what the dock's click on a hidden or minimized task does.
    // A hidden application comes back whole.
    getUserDataAs<WindowResourceData>(resource)->management->windowManager()->unhideApplication(
            window);
    window->setHidden(false);
    window = resolve(resource);
    if (window && window->state()->isMinimized())
        window->restore();
    window = resolve(resource);
    if (window)
        window->manager()->activation()->activate(window);
}

void windowMinimize(wl_client*, wl_resource* resource)
{
    if (wm::Window* window = resolve(resource))
        window->minimize();
}

void windowRestore(wl_client*, wl_resource* resource)
{
    if (wm::Window* window = resolve(resource))
        window->restore();
}

void windowClose(wl_client*, wl_resource* resource)
{
    if (wm::Window* window = resolve(resource))
        window->requestClose();
}

void windowToggleMaximize(wl_client*, wl_resource* resource)
{
    wm::Window* window = resolve(resource);
    if (!window)
        return;
    if (window->stateType() == wm::WindowStateType::Maximized)
        window->restore();
    else
        window->maximize();
}

void windowStartMove(wl_client*, wl_resource* resource)
{
    wm::Window* window = resolve(resource);
    if (!window)
        return;
    wm::WindowManager* manager = window->manager();
    input::Seat* seat = manager->seat();
    if (!seat)
        return;
    manager->eventHandler()->beginMove(window, seat->pointerLocation());
}

void windowSetMinimizedGeometry(wl_client*, wl_resource* resource, wl_resource* panelResource,
                                int32_t x, int32_t y, int32_t width, int32_t height)
{
    wm::Window* window = resolve(resource);
    if (!window)
        return;
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_V1_ERROR_INVALID_GEOMETRY,
                               "negative minimized geometry %dx%d", width, height);
        return;
    }

    if (width == 0 || height == 0) {
        window->setMinimizedGeometry(QRect());
        return;
    }

    compositor::Surface* panel = surfaceFromResource(panelResource);
    wm::Window* panelWindow = panel ? window->manager()->windowForSurface(panel) : nullptr;
    if (!panelWindow) {
        // The dock's own window is not mapped yet; the shell will send this
        // again once it is.
        window->setMinimizedGeometry(QRect());
        return;
    }

    window->setMinimizedGeometry(panelWindow, clientRect(x, y, width, height));
}

void windowHandoff(wl_client*, wl_resource* resource)
{
    if (wm::Window* window = resolve(resource))
        window->endMinimizeHandoff();
}

void windowHide(wl_client*, wl_resource* resource)
{
    if (wm::Window* window = resolve(resource))
        window->setHidden(true);
}

void windowUnhide(wl_client*, wl_resource* resource)
{
    if (wm::Window* window = resolve(resource))
        window->setHidden(false);
}

void windowMoveToDesk(wl_client*, wl_resource* resource, uint32_t deskId)
{
    wm::Window* window = resolve(resource);
    if (!window)
        return;
    wm::DesksController* desks = window->manager()->desks();
    if (desks->areDesksBeingModified())
        return;
    if (deskId == 0) {
        desks->setVisibleOnAllDesks(window, true);
        return;
    }
    if (wm::Desk* desk = desks->deskById(deskId))
        desks->sendToDeskAtIndex(window, desks->deskIndex(desk));
}

void windowForceQuit(wl_client*, wl_resource* resource)
{
    if (wm::Window* window = resolve(resource))
        window->manager()->forceQuit(window);
}

const struct cutefish_window_v1_interface kWindowImplementation = {
    windowDestroy,
    windowActivate,
    windowMinimize,
    windowRestore,
    windowClose,
    windowToggleMaximize,
    windowStartMove,
    windowSetMinimizedGeometry,
    windowHandoff,
    windowHide,
    windowUnhide,
    windowMoveToDesk,
    windowForceQuit,
};

void managementDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void managementQuitApplication(wl_client*, wl_resource* resource, wl_resource* window)
{
    auto* management = getUserDataAs<WaylandCutefishWindowManagement>(resource);
    if (management)
        management->windowManager()->quitApplication(resolve(window));
}

void managementHideApplication(wl_client*, wl_resource* resource, wl_resource* window)
{
    auto* management = getUserDataAs<WaylandCutefishWindowManagement>(resource);
    if (management)
        management->windowManager()->hideApplication(resolve(window));
}

void managementHideOtherApplications(wl_client*, wl_resource* resource, wl_resource* window)
{
    auto* management = getUserDataAs<WaylandCutefishWindowManagement>(resource);
    wm::Window* keep = window ? resolve(window) : nullptr;
    // A window that is already gone must not turn into "hide everything".
    if (management && (keep || !window))
        management->windowManager()->hideOtherApplications(keep);
}

void managementPick(wl_client*, wl_resource* resource, uint32_t serial, int32_t x, int32_t y)
{
    if (auto* management = getUserDataAs<WaylandCutefishWindowManagement>(resource))
        management->pick(resource, serial, QPoint(x, y));
}

wm::DesksController* desksOf(wl_resource* resource)
{
    auto* management = getUserDataAs<WaylandCutefishWindowManagement>(resource);
    return management ? management->windowManager()->desks() : nullptr;
}

void managementActivateDesk(wl_client*, wl_resource* resource, uint32_t deskId)
{
    wm::DesksController* desks = desksOf(resource);
    if (!desks || desks->areDesksBeingModified())
        return;
    if (wm::Desk* desk = desks->deskById(deskId))
        desks->activateDesk(desk, wm::DesksSwitchSource::ApiSwitch);
}

void managementCreateDesk(wl_client*, wl_resource* resource, uint32_t activate)
{
    wm::DesksController* desks = desksOf(resource);
    if (!desks || desks->areDesksBeingModified())
        return;
    wm::Desk* desk = desks->newDesk(wm::DesksCreationRemovalSource::Api);
    if (desk && activate)
        desks->activateDesk(desk, wm::DesksSwitchSource::ApiSwitch);
}

void managementRemoveDesk(wl_client*, wl_resource* resource, uint32_t deskId)
{
    wm::DesksController* desks = desksOf(resource);
    if (!desks || desks->areDesksBeingModified())
        return;
    if (wm::Desk* desk = desks->deskById(deskId))
        desks->removeDesk(desk, wm::DesksCreationRemovalSource::Api);
}

void managementSetDeskName(wl_client*, wl_resource* resource, uint32_t deskId, const char* name)
{
    wm::DesksController* desks = desksOf(resource);
    if (!desks)
        return;
    if (wm::Desk* desk = desks->deskById(deskId))
        desks->setDeskName(desk, QString::fromUtf8(name));
}

const struct cutefish_window_management_v1_interface kManagementImplementation = {
    managementDestroy,
    managementQuitApplication,
    managementHideApplication,
    managementHideOtherApplications,
    managementPick,
    managementActivateDesk,
    managementCreateDesk,
    managementRemoveDesk,
    managementSetDeskName,
};

void onManagementResourceDestroyed(wl_resource* resource)
{
    auto* management = getUserDataAs<WaylandCutefishWindowManagement>(resource);
    if (management)
        management->removeManagerResource(resource);
}

void bindManagement(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto* management = static_cast<WaylandCutefishWindowManagement*>(data);

    // Defence in depth behind the global filter.
    if (!management->privileged()->isTrusted(client)) {
        wl_client_post_implementation_error(client,
                                            "cutefish_window_management_v1 is not available");
        return;
    }

    wl_resource* resource =
        wl_resource_create(client, &cutefish_window_management_v1_interface,
                           std::min(version, kCutefishWindowManagementVersion), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kManagementImplementation, management,
                                   onManagementResourceDestroyed);
    management->addManagerResource(resource);
    management->sendInitialState(resource);
}

} // namespace

WaylandCutefishWindowManagement::WaylandCutefishWindowManagement(wm::WindowManager* windowManager,
                                                                 PrivilegedClients* privileged,
                                                                 QObject* parent)
    : QObject(parent)
    , m_windowManager(windowManager)
    , m_privileged(privileged)
{
    connect(m_windowManager, &wm::WindowManager::windowAdded, this,
            &WaylandCutefishWindowManagement::onWindowAdded);
    connect(m_windowManager, &wm::WindowManager::windowRemoved, this,
            &WaylandCutefishWindowManagement::onWindowRemoved);
    connect(m_windowManager, &wm::WindowManager::activeWindowChanged, this,
            &WaylandCutefishWindowManagement::onActiveWindowChanged);

    m_windowManager->desks()->addObserver(this);
    m_windowManager->setNotRespondingPrompt([this](wm::Window* window) {
        const std::vector<wl_resource*> resources = resourcesFor(window);
        for (wl_resource* resource : resources)
            cutefish_window_v1_send_not_responding(resource);
        return !resources.empty();
    });

    // Windows that already exist - the compositor may have been running for a
    // while before the protocol was registered.
    for (wm::Window* window : m_windowManager->windows())
        onWindowAdded(window);
    onActiveWindowChanged(m_windowManager->activation()->activeWindow());
}

WaylandCutefishWindowManagement::~WaylandCutefishWindowManagement()
{
    m_windowManager->setNotRespondingPrompt({});
    m_windowManager->desks()->removeObserver(this);
    for (auto* window : m_observedWindows) {
        window->removeObserver(this);
        if (window->rootSurface())
            window->rootSurface()->removeSurfaceObserver(this);
    }
    m_windowResources.clear();
}

void WaylandCutefishWindowManagement::onWindowAdded(wm::Window* window)
{
    if (!window || window->isShellWindow() || window->isPopup())
        return;
    if (!m_observedWindows.insert(window).second)
        return;

    // Keep watching excluded app IDs so a later change can restore the task.
    window->addObserver(this);
    if (window->rootSurface())
        window->rootSurface()->addSurfaceObserver(this);
    announceWindow(window);
}

void WaylandCutefishWindowManagement::announceWindow(wm::Window* window)
{
    if (window->isShellApplication() || m_windowResources.count(window))
        return;
    m_windowResources.emplace(window, std::vector<wl_resource*>());

    for (wl_resource* manager : std::vector<wl_resource*>(m_managers)) {
        wl_client* client = wl_resource_get_client(manager);
        wl_resource* resource = wl_resource_create(client, &cutefish_window_v1_interface,
                                                   wl_resource_get_version(manager), 0);
        if (!resource) {
            wl_client_post_no_memory(client);
            continue;
        }
        setImplementation(resource, &kWindowImplementation,
                          std::make_unique<WindowResourceData>(
                              WindowResourceData {this, window->id()}));
        m_windowResources[window].push_back(resource);
        cutefish_window_management_v1_send_window(manager, resource, idHigh(window->id()),
                                                  idLow(window->id()));
        sendThumbnail(resource, window);
        sendState(resource, window);
    }
}

void WaylandCutefishWindowManagement::onWindowRemoved(wm::Window* window)
{
    if (!m_observedWindows.erase(window))
        return;
    window->removeObserver(this);
    if (window->rootSurface())
        window->rootSurface()->removeSurfaceObserver(this);
    closeWindowResources(window);
    onActiveWindowChanged(m_windowManager->activation()->activeWindow());
}

void WaylandCutefishWindowManagement::closeWindowResources(wm::Window* window)
{
    const auto it = m_windowResources.find(window);
    if (it == m_windowResources.end())
        return;

    // Out of the map before the client is told, so a re-entrant lookup from
    // inside the event cannot find a window that is on its way out. The
    // resources stay alive: the shell destroys them itself.
    std::vector<wl_resource*> resources = std::move(it->second);
    m_windowResources.erase(it);

    for (wl_resource* resource : resources) {
        if (auto* data = getUserDataAs<WindowResourceData>(resource))
            data->windowId = 0;
        cutefish_window_v1_send_closed(resource);
    }
}

void WaylandCutefishWindowManagement::onActiveWindowChanged(wm::Window* window)
{
    std::erase_if(m_applicationHistory, [this](quint64 id) {
        return !m_windowResources.count(m_windowManager->windowById(id));
    });
    // A sheet speaks for the window it hangs from.
    if (window && window->role() == wm::WindowRole::Sheet && window->transientParent())
        window = window->transientParent();
    if (!window || window->isShellWindow() || window->isPopup()) {
        m_applicationHistory.clear();
    } else if (m_windowResources.count(window)) {
        std::erase(m_applicationHistory, window->id());
        m_applicationHistory.insert(m_applicationHistory.begin(), window->id());
    }
    const quint64 id = m_applicationHistory.empty() ? 0 : m_applicationHistory.front();
    if (id == m_activeWindowId)
        return;
    m_activeWindowId = id;
    for (wl_resource* manager : std::vector<wl_resource*>(m_managers))
        cutefish_window_management_v1_send_active_window(manager, idHigh(id), idLow(id));
}

void WaylandCutefishWindowManagement::addManagerResource(wl_resource* manager)
{
    if (std::find(m_managers.begin(), m_managers.end(), manager) == m_managers.end())
        m_managers.push_back(manager);
}

void WaylandCutefishWindowManagement::removeManagerResource(wl_resource* manager)
{
    m_managers.erase(std::remove(m_managers.begin(), m_managers.end(), manager), m_managers.end());

    // The window resources this manager created belong to the same client and
    // are being destroyed with it; drop the references so nothing writes to
    // them afterwards.
    for (auto& entry : m_windowResources) {
        std::vector<wl_resource*>& resources = entry.second;
        resources.erase(std::remove_if(resources.begin(), resources.end(),
                                       [manager](wl_resource* resource) {
                                           return wl_resource_get_client(resource)
                                               == wl_resource_get_client(manager);
                                       }),
                        resources.end());
    }
}

void WaylandCutefishWindowManagement::sendInitialState(wl_resource* manager)
{
    wl_client* client = wl_resource_get_client(manager);

    // The desks first, so every window's desk is one the shell knows.
    sendDesks(manager);

    // Bottom to top, so the shell sees the same order the compositor stacks in.
    for (wm::Window* window : m_windowManager->windows()) {
        const auto it = m_windowResources.find(window);
        if (it == m_windowResources.end())
            continue;

        wl_resource* resource = wl_resource_create(client, &cutefish_window_v1_interface,
                                                   wl_resource_get_version(manager), 0);
        if (!resource) {
            wl_client_post_no_memory(client);
            return;
        }
        setImplementation(resource, &kWindowImplementation,
                          std::make_unique<WindowResourceData>(
                              WindowResourceData {this, window->id()}));
        it->second.push_back(resource);
        cutefish_window_management_v1_send_window(manager, resource, idHigh(window->id()),
                                                  idLow(window->id()));
        sendThumbnail(resource, window);
        sendState(resource, window);
    }

    cutefish_window_management_v1_send_active_window(manager, idHigh(m_activeWindowId),
                                                      idLow(m_activeWindowId));
    cutefish_window_management_v1_send_initial_state_done(manager);
}

void WaylandCutefishWindowManagement::pick(wl_resource* manager, uint32_t serial,
                                           const QPoint& position)
{
    uint32_t kind = CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_NONE;
    quint64 id = 0;
    if (wm::Window* window = m_windowManager->capturableWindowAt(position)) {
        switch (window->role()) {
        case wm::WindowRole::Normal:
            if (m_windowResources.count(window)) {
                kind = CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_WINDOW;
                id = window->id();
            }
            break;
        case wm::WindowRole::Desktop:
            kind = CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_DESKTOP;
            break;
        case wm::WindowRole::Panel:
            kind = CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_PANEL;
            break;
        case wm::WindowRole::Dock:
            kind = CUTEFISH_WINDOW_MANAGEMENT_V1_PICKED_KIND_DOCK;
            break;
        default:
            break;
        }
    }
    cutefish_window_management_v1_send_picked(manager, serial, kind, idHigh(id), idLow(id));
}

std::vector<wl_resource*> WaylandCutefishWindowManagement::resourcesFor(wm::Window* window) const
{
    const auto it = m_windowResources.find(window);
    return it == m_windowResources.end() ? std::vector<wl_resource*>() : it->second;
}

void WaylandCutefishWindowManagement::sendState(wl_resource* resource, wm::Window* window)
{
    cutefish_window_v1_send_skip_taskbar(resource, window->skipTaskbar());
    const auto menu = window->rootSurface() ? window->rootSurface()->menuAddress()
                                            : compositor::Surface::MenuAddress{};
    cutefish_window_v1_send_application_menu(resource, menu.service.toUtf8().constData(),
                                            menu.path.toUtf8().constData());
    cutefish_window_v1_send_title(resource, window->title().toUtf8().constData());
    cutefish_window_v1_send_app_id(resource, window->applicationId().toUtf8().constData());
    cutefish_window_v1_send_pid(resource, static_cast<uint32_t>(window->processId()));
    sendParent(resource, window);
    sendDesk(resource, window);

    const wm::WindowStateType stateType = window->stateType();
    cutefish_window_v1_send_state(resource, window->isActive() ? 1 : 0,
                                  stateType == wm::WindowStateType::Minimized ? 1 : 0,
                                  stateType == wm::WindowStateType::Maximized ? 1 : 0,
                                  stateType == wm::WindowStateType::Fullscreen ? 1 : 0,
                                  window->isHidden() ? 1 : 0);
    cutefish_window_v1_send_responsive(resource, window->isResponsive() ? 1 : 0);

    const QRect bounds = window->bounds();
    cutefish_window_v1_send_geometry(resource, bounds.x(), bounds.y(), bounds.width(),
                                     bounds.height());
    cutefish_window_v1_send_done(resource);
}

void WaylandCutefishWindowManagement::sendDesks(wl_resource* manager)
{
    const wm::DesksController* desks = m_windowManager->desks();
    for (const auto& desk : desks->desks()) {
        cutefish_window_management_v1_send_desk(manager, desk->id());
        if (desk->isNameSetByUser())
            cutefish_window_management_v1_send_desk_name(manager, desk->id(),
                                                         desk->name().toUtf8().constData());
    }
    cutefish_window_management_v1_send_active_desk(manager, desks->activeDesk()->id());
}

void WaylandCutefishWindowManagement::sendDesk(wl_resource* resource, wm::Window* window)
{
    const wm::Desk* desk = m_windowManager->desks()->deskOf(window);
    cutefish_window_v1_send_desk(resource, desk ? desk->id() : 0);
}

void WaylandCutefishWindowManagement::onDeskAdded(const wm::Desk* desk)
{
    for (wl_resource* manager : std::vector<wl_resource*>(m_managers))
        cutefish_window_management_v1_send_desk(manager, desk->id());
}

void WaylandCutefishWindowManagement::onDeskRemoved(const wm::Desk* desk)
{
    for (wl_resource* manager : std::vector<wl_resource*>(m_managers))
        cutefish_window_management_v1_send_desk_removed(manager, desk->id());
}

void WaylandCutefishWindowManagement::onDeskActivationChanged(const wm::Desk* activated,
                                                              const wm::Desk*)
{
    for (wl_resource* manager : std::vector<wl_resource*>(m_managers))
        cutefish_window_management_v1_send_active_desk(manager, activated->id());
}

void WaylandCutefishWindowManagement::onDeskNameChanged(const wm::Desk* desk)
{
    const QByteArray name = desk->isNameSetByUser() ? desk->name().toUtf8() : QByteArray();
    for (wl_resource* manager : std::vector<wl_resource*>(m_managers))
        cutefish_window_management_v1_send_desk_name(manager, desk->id(), name.constData());
}

void WaylandCutefishWindowManagement::onWindowDeskChanged(wm::Window* window)
{
    for (wl_resource* resource : resourcesFor(window)) {
        sendDesk(resource, window);
        cutefish_window_v1_send_done(resource);
    }
}

void WaylandCutefishWindowManagement::sendParent(wl_resource* resource, wm::Window* window)
{
    const quint64 parent = window->transientParent() ? window->transientParent()->id() : 0;
    cutefish_window_v1_send_parent(resource, idHigh(parent), idLow(parent));
}

void WaylandCutefishWindowManagement::sendThumbnail(wl_resource* resource, wm::Window* window)
{
    const QImage& snapshot = window->minimizedSnapshot();
    if (snapshot.isNull() || !window->state()->isMinimized())
        return;
    const int fd = thumbnailFile(snapshot);
    if (fd < 0) {
        qCWarning(cfCompositor) << "cannot share the minimized thumbnail of" << window->title();
        return;
    }
    // libwayland duplicates the descriptor into the message.
    cutefish_window_v1_send_thumbnail(resource, fd, uint32_t(snapshot.width()),
                                      uint32_t(snapshot.height()));
    close(fd);
}

void WaylandCutefishWindowManagement::onSurfaceWindowHintsChanged(compositor::Surface* surface)
{
    auto* window = m_windowManager->windowForSurface(surface);
    if (!window || window->rootSurface() != surface)
        return;
    for (auto* resource : resourcesFor(window))
        sendState(resource, window);
}

void WaylandCutefishWindowManagement::onSurfaceMenuChanged(compositor::Surface* surface)
{
    auto* window = m_windowManager->windowForSurface(surface);
    if (!window || window->rootSurface() != surface)
        return;
    const auto& menu = surface->menuAddress();
    for (auto* resource : resourcesFor(window)) {
        cutefish_window_v1_send_application_menu(resource, menu.service.toUtf8().constData(),
                                                menu.path.toUtf8().constData());
        cutefish_window_v1_send_done(resource);
    }
}

void WaylandCutefishWindowManagement::onSurfaceDestroying(compositor::Surface* surface)
{
    surface->removeSurfaceObserver(this);
}

void WaylandCutefishWindowManagement::onWindowTitleChanged(wm::Window* window)
{
    for (wl_resource* resource : resourcesFor(window)) {
        cutefish_window_v1_send_title(resource, window->title().toUtf8().constData());
        cutefish_window_v1_send_done(resource);
    }
}

void WaylandCutefishWindowManagement::onWindowApplicationIdChanged(wm::Window* window)
{
    if (window->isShellApplication()) {
        closeWindowResources(window);
    } else if (!m_windowResources.count(window)) {
        announceWindow(window);
    } else {
        for (wl_resource* resource : resourcesFor(window)) {
            cutefish_window_v1_send_app_id(resource, window->applicationId().toUtf8().constData());
            cutefish_window_v1_send_done(resource);
        }
    }
    onActiveWindowChanged(m_windowManager->activation()->activeWindow());
}

void WaylandCutefishWindowManagement::onWindowBoundsChanged(wm::Window* window, const QRect&)
{
    const QRect bounds = window->bounds();
    for (wl_resource* resource : resourcesFor(window)) {
        cutefish_window_v1_send_geometry(resource, bounds.x(), bounds.y(), bounds.width(),
                                         bounds.height());
        cutefish_window_v1_send_done(resource);
    }
}

void WaylandCutefishWindowManagement::onWindowStateTypeChanged(wm::Window* window,
                                                                wm::WindowStateType)
{
    for (wl_resource* resource : resourcesFor(window))
        sendState(resource, window);
}

void WaylandCutefishWindowManagement::onWindowActivationChanged(wm::Window* window, bool)
{
    for (wl_resource* resource : resourcesFor(window))
        sendState(resource, window);
}

void WaylandCutefishWindowManagement::onWindowTransientParentChanged(wm::Window* window,
                                                                     wm::Window*)
{
    for (wl_resource* resource : resourcesFor(window)) {
        sendParent(resource, window);
        cutefish_window_v1_send_done(resource);
    }
}

void WaylandCutefishWindowManagement::onWindowResponsiveChanged(wm::Window* window)
{
    for (wl_resource* resource : resourcesFor(window)) {
        cutefish_window_v1_send_responsive(resource, window->isResponsive() ? 1 : 0);
        cutefish_window_v1_send_done(resource);
    }
}

void WaylandCutefishWindowManagement::onWindowHiddenChanged(wm::Window* window)
{
    for (wl_resource* resource : resourcesFor(window))
        sendState(resource, window);
}

void WaylandCutefishWindowManagement::onWindowMinimizedSnapshotChanged(wm::Window* window)
{
    if (window->minimizedSnapshot().isNull())
        return;
    // One file for every resource: each send duplicates the descriptor.
    const int fd = thumbnailFile(window->minimizedSnapshot());
    if (fd < 0)
        return;
    const QImage& snapshot = window->minimizedSnapshot();
    for (wl_resource* resource : resourcesFor(window)) {
        cutefish_window_v1_send_thumbnail(resource, fd, uint32_t(snapshot.width()),
                                          uint32_t(snapshot.height()));
        cutefish_window_v1_send_done(resource);
    }
    close(fd);
}

void WaylandCutefishWindowManagement::onWindowMinimizeLanded(wm::Window* window)
{
    const std::vector<wl_resource*> resources = resourcesFor(window);
    // Nobody to hand over to.
    if (resources.empty()) {
        window->endMinimizeHandoff();
        return;
    }
    for (wl_resource* resource : resources)
        cutefish_window_v1_send_minimize_landed(resource);
}

void WaylandCutefishWindowManagement::onWindowDestroying(wm::Window* window)
{
    // Normally windowRemoved has already run; this catches a window torn down
    // without going through the window manager.
    onWindowRemoved(window);
}

wl_global* createCutefishWindowManagementGlobal(wl_display* display,
                                                WaylandCutefishWindowManagement* management)
{
    wl_global* global =
        wl_global_create(display, &cutefish_window_management_v1_interface,
                         kCutefishWindowManagementVersion, management, bindManagement);
    if (!global)
        return nullptr;
    management->privileged()->addPrivateGlobal(global);
    return global;
}

} // namespace cutefish::wayland
