// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/cutefish_shell.h"

#include <cutefish-shell-v1-server-protocol.h>

#include <algorithm>
#include <memory>
#include <optional>

#include "base/strings.h"
#include "compositor/surface.h"
#include "compositor/surface_observer.h"
#include "core/logging.h"
#include "display/display_manager.h"
#include "input/drag.h"
#include "input/seat.h"
#include "wayland/privileged_client.h"
#include "wayland/server_util.h"
#include "wm/activation.h"
#include "wm/window.h"
#include "wm/window_manager.h"

namespace cutefish::wayland {

namespace {

using cutefish::wm::FocusPolicy;
using cutefish::wm::PanelBehavior;
using cutefish::wm::ScreenEdge;
using cutefish::wm::ShellWindowMetadata;
using cutefish::wm::WindowRole;

std::optional<WindowRole> roleFromWayland(uint32_t role)
{
    switch (role) {
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_DESKTOP:
        return WindowRole::Desktop;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_PANEL:
        return WindowRole::Panel;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_DOCK:
        return WindowRole::Dock;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_LAUNCHER:
        return WindowRole::Launcher;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_NOTIFICATION:
        return WindowRole::Notification;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_CAPTURE:
        return WindowRole::Capture;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_OSD:
        return WindowRole::Osd;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_SHEET:
        return WindowRole::Sheet;
    case CUTEFISH_SHELL_SURFACE_V1_ROLE_PICKER:
        return WindowRole::Picker;
    default:
        return std::nullopt;
    }
}

std::optional<ScreenEdge> edgeFromWayland(uint32_t edge)
{
    switch (edge) {
    case CUTEFISH_SHELL_SURFACE_V1_EDGE_TOP:
        return ScreenEdge::Top;
    case CUTEFISH_SHELL_SURFACE_V1_EDGE_BOTTOM:
        return ScreenEdge::Bottom;
    case CUTEFISH_SHELL_SURFACE_V1_EDGE_LEFT:
        return ScreenEdge::Left;
    case CUTEFISH_SHELL_SURFACE_V1_EDGE_RIGHT:
        return ScreenEdge::Right;
    default:
        return std::nullopt;
    }
}

// The user data of a cutefish_shell_surface_v1. It owns no window and no
// surface: it holds the shell's description of one surface and pushes it into
// the window manager, which is the only thing that outlives a shell restart.
class CutefishShellSurface : public compositor::SurfaceObserver
{
public:
    CutefishShellSurface(wm::WindowManager* manager, compositor::Surface* surface)
        : m_manager(manager)
        , m_surface(surface)
    {
        m_surface->addSurfaceObserver(this);
    }

    ~CutefishShellSurface() override
    {
        // The reservation goes first: releasing work area is visible to every
        // other window and must not depend on the surface still existing.
        m_manager->clearExclusiveZone(this);
        if (m_surface) {
            m_surface->removeSurfaceObserver(this);
            // Only the stored metadata is dropped. A window that is still
            // mapped keeps the role it was given: Qt destroys and recreates
            // this object every time it recreates the wl_surface, and pushing
            // a Normal role into the live window in between would drop the
            // dock and the panels out of their stacking layer.
            m_manager->clearShellMetadata(m_surface);
        }
    }

    compositor::Surface* surface() const { return m_surface; }

    void setRole(WindowRole role)
    {
        m_metadata.role = role;
        apply();
    }

    void setPosition(const base::Point& position)
    {
        m_metadata.position = position;
        m_metadata.hasPosition = true;
        apply();
    }

    void setPanelBehavior(PanelBehavior behavior)
    {
        m_metadata.panelBehavior = behavior;
        apply();
    }

    void setFocusPolicy(FocusPolicy policy)
    {
        m_metadata.focusPolicy = policy;
        apply();
    }

    void setExclusiveZone(ScreenEdge edge, int thickness)
    {
        m_manager->setExclusiveZone(this, edge, thickness, m_metadata.outputName);
    }

    bool setOutput(const std::string& name)
    {
        auto* displays = m_manager->displayManager();
        if (!displays || !displays->applied())
            return false;
        const auto& logical = displays->applied()->logicalDisplays;
        if (std::none_of(logical.begin(), logical.end(), [&](const auto& output) {
                return output.waylandName == name;
            })) {
            return false;
        }
        m_metadata.outputName = name;
        apply();
        return true;
    }

    void clearExclusiveZone() { m_manager->clearExclusiveZone(this); }

    void setParent(uint64_t parentWindow)
    {
        m_metadata.parentWindow = parentWindow;
        apply();
    }

    void activate()
    {
        if (!m_surface)
            return;
        wm::Window* window = m_manager->windowForSurface(m_surface);
        if (!window || window->rootSurface() != m_surface)
            return;
        if (window->role() == wm::WindowRole::Capture
            || window->role() == wm::WindowRole::Picker)
            m_manager->setKeyboardGrab(window);
        else
            m_manager->activation()->activate(window);
    }

    // compositor::SurfaceObserver
    void onSurfaceDestroying(compositor::Surface* surface) override
    {
        if (surface != m_surface)
            return;
        m_manager->clearShellMetadata(m_surface);
        m_manager->clearExclusiveZone(this);
        m_surface = nullptr;
    }

private:
    void apply()
    {
        if (m_surface)
            m_manager->setShellMetadata(m_surface, m_metadata);
    }

    wm::WindowManager* m_manager = nullptr;
    compositor::Surface* m_surface = nullptr;
    ShellWindowMetadata m_metadata;
};

void shellSurfaceDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void shellSurfaceSetRole(wl_client*, wl_resource* resource, uint32_t role)
{
    const std::optional<WindowRole> parsed = roleFromWayland(role);
    if (!parsed) {
        wl_resource_post_error(resource, CUTEFISH_SHELL_SURFACE_V1_ERROR_INVALID_ROLE,
                               "unknown role %u", role);
        return;
    }
    getUserDataAs<CutefishShellSurface>(resource)->setRole(*parsed);
}

void shellSurfaceSetPosition(wl_client*, wl_resource* resource, int32_t x, int32_t y)
{
    getUserDataAs<CutefishShellSurface>(resource)->setPosition(base::Point(x, y));
}

void shellSurfaceSetPanelBehavior(wl_client*, wl_resource* resource, uint32_t behavior)
{
    PanelBehavior parsed = PanelBehavior::StayAbove;
    switch (behavior) {
    case CUTEFISH_SHELL_SURFACE_V1_PANEL_BEHAVIOR_STAY_ABOVE:
        parsed = PanelBehavior::StayAbove;
        break;
    case CUTEFISH_SHELL_SURFACE_V1_PANEL_BEHAVIOR_WINDOWS_CAN_COVER:
        parsed = PanelBehavior::WindowsCanCover;
        break;
    default:
        wl_resource_post_error(resource, CUTEFISH_SHELL_SURFACE_V1_ERROR_INVALID_BEHAVIOR,
                               "unknown panel behavior %u", behavior);
        return;
    }
    getUserDataAs<CutefishShellSurface>(resource)->setPanelBehavior(parsed);
}

void shellSurfaceSetFocusPolicy(wl_client*, wl_resource* resource, uint32_t policy)
{
    FocusPolicy parsed = FocusPolicy::Normal;
    switch (policy) {
    case CUTEFISH_SHELL_SURFACE_V1_FOCUS_POLICY_NORMAL:
        parsed = FocusPolicy::Normal;
        break;
    case CUTEFISH_SHELL_SURFACE_V1_FOCUS_POLICY_NO_FOCUS:
        parsed = FocusPolicy::NoFocus;
        break;
    default:
        wl_resource_post_error(resource, CUTEFISH_SHELL_SURFACE_V1_ERROR_INVALID_FOCUS_POLICY,
                               "unknown focus policy %u", policy);
        return;
    }
    getUserDataAs<CutefishShellSurface>(resource)->setFocusPolicy(parsed);
}

void shellSurfaceSetExclusiveZone(wl_client*, wl_resource* resource, uint32_t edge,
                                  int32_t thickness)
{
    const std::optional<ScreenEdge> parsed = edgeFromWayland(edge);
    if (!parsed) {
        wl_resource_post_error(resource, CUTEFISH_SHELL_SURFACE_V1_ERROR_INVALID_EDGE,
                               "unknown edge %u", edge);
        return;
    }
    getUserDataAs<CutefishShellSurface>(resource)->setExclusiveZone(*parsed, thickness);
}

void shellSurfaceClearExclusiveZone(wl_client*, wl_resource* resource)
{
    getUserDataAs<CutefishShellSurface>(resource)->clearExclusiveZone();
}

void shellSurfaceActivate(wl_client*, wl_resource* resource)
{
    getUserDataAs<CutefishShellSurface>(resource)->activate();
}

void shellSurfaceSetOutput(wl_client*, wl_resource* resource, const char* name)
{
    auto* surface = getUserDataAs<CutefishShellSurface>(resource);
    if (!surface->setOutput(base::strings::sanitizedUtf8(name))) {
        wl_resource_post_error(resource, CUTEFISH_SHELL_SURFACE_V1_ERROR_INVALID_OUTPUT,
                               "unknown logical output %s", name);
    }
}

void shellSurfaceSetParent(wl_client*, wl_resource* resource, uint32_t idHigh, uint32_t idLow)
{
    getUserDataAs<CutefishShellSurface>(resource)->setParent(
            (static_cast<uint64_t>(idHigh) << 32) | idLow);
}

const struct cutefish_shell_surface_v1_interface kShellSurfaceImplementation = {
    shellSurfaceDestroy,
    shellSurfaceSetRole,
    shellSurfaceSetPosition,
    shellSurfaceSetPanelBehavior,
    shellSurfaceSetFocusPolicy,
    shellSurfaceSetExclusiveZone,
    shellSurfaceClearExclusiveZone,
    shellSurfaceActivate,
    shellSurfaceSetOutput,
    shellSurfaceSetParent,
};

// The user data of a cutefish_shell_drag_v1. It follows one drag of its own
// client: the one in progress when it is created, or else the next one.
class CutefishShellDrag
{
public:
    CutefishShellDrag(input::DragController* drag, wl_resource* resource)
        : m_drag(drag->weakPtr())
        , m_resource(resource)
    {
        m_startedConnection = drag->started.connect([this] {
            follow();
        });
        if (m_drag->active())
            follow();
    }

    ~CutefishShellDrag()
    {
        if (m_holdsReturn && m_drag)
            m_drag->releaseReturn();
        // Gone mid-drag: nobody would release the icon sent home, nor restore the rest.
        if (m_following && m_drag && m_drag->active()) {
            m_drag->setReturnsHome(false);
            m_drag->setIconOpacity(1);
            m_drag->setPlainIdleCursor(false);
        }
    }

    void setIconOpacity(float opacity)
    {
        m_iconOpacity = opacity;
        if (m_following && m_drag)
            m_drag->setIconOpacity(opacity);
    }

    void setReturnsHome(bool returns)
    {
        m_returnsHome = returns;
        if (m_following && m_drag)
            m_drag->setReturnsHome(returns);
    }

    void setHome(const base::PointF& home)
    {
        m_home = home;
        if (m_following && m_drag)
            m_drag->setHome(home);
    }

private:
    bool ownsDrag() const
    {
        wl_resource* origin = getSurfaceResource(m_drag->origin());
        return origin && wl_resource_get_client(origin) == wl_resource_get_client(m_resource);
    }

    void follow()
    {
        if (m_following || !m_drag || !m_drag->active() || m_drag->isExternal() || !ownsDrag())
            return;
        m_following = true;
        m_startedConnection.reset();

        m_dragConnections.push_back(m_drag->locationChanged.connect(
                [this](const base::PointF& location) { sendMotion(location); }));
        m_dragConnections.push_back(m_drag->released.connect([this] {
            m_released = true;
            const base::PointF location = m_drag->location();
            cutefish_shell_drag_v1_send_dropped(m_resource, wl_fixed_from_double(location.x()),
                                                wl_fixed_from_double(location.y()));
        }));
        m_dragConnections.push_back(m_drag->ended.connect([this] {
            const bool returning = m_drag->isReturning();
            m_dragConnections.clear();
            // A later drag is not this one: settings no longer reach it.
            m_following = false;
            if (!m_released)
                cutefish_shell_drag_v1_send_cancelled(m_resource);
            m_holdsReturn = returning;
            if (returning) {
                m_dragConnections.push_back(m_drag->returned.connect([this] {
                    m_dragConnections.clear();
                    cutefish_shell_drag_v1_send_returned(m_resource);
                }));
            }
        }));

        m_drag->setPlainIdleCursor(true);
        m_drag->setReturnsHome(m_returnsHome);
        if (m_home)
            m_drag->setHome(*m_home);
        if (m_iconOpacity)
            m_drag->setIconOpacity(*m_iconOpacity);
        sendMotion(m_drag->location());
    }

    void sendMotion(const base::PointF& location)
    {
        cutefish_shell_drag_v1_send_motion(m_resource, wl_fixed_from_double(location.x()),
                                           wl_fixed_from_double(location.y()));
    }

    base::WeakPtr<input::DragController> m_drag;
    wl_resource* m_resource = nullptr;
    base::Subscription m_startedConnection;
    std::vector<base::Subscription> m_dragConnections;
    std::optional<float> m_iconOpacity;
    bool m_returnsHome = false;
    std::optional<base::PointF> m_home;
    // The icon of the drag followed is coming back, or waits at home for
    // this object to go.
    bool m_holdsReturn = false;
    bool m_following = false;
    bool m_released = false;
};

void dragDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void dragSetIconOpacity(wl_client*, wl_resource* resource, wl_fixed_t opacity)
{
    getUserDataAs<CutefishShellDrag>(resource)->setIconOpacity(
            static_cast<float>(wl_fixed_to_double(opacity)));
}

void dragSetReturnsHome(wl_client*, wl_resource* resource, uint32_t returns)
{
    getUserDataAs<CutefishShellDrag>(resource)->setReturnsHome(returns != 0);
}

void dragSetHome(wl_client*, wl_resource* resource, wl_fixed_t x, wl_fixed_t y)
{
    getUserDataAs<CutefishShellDrag>(resource)->setHome(
            base::PointF(wl_fixed_to_double(x), wl_fixed_to_double(y)));
}

const struct cutefish_shell_drag_v1_interface kDragImplementation = {
    dragDestroy,
    dragSetIconOpacity,
    dragSetReturnsHome,
    dragSetHome,
};

void shellDestroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

void shellGetShellSurface(wl_client* client, wl_resource* resource, uint32_t id,
                          wl_resource* surfaceResource)
{
    auto* shell = getUserDataAs<WaylandCutefishShell>(resource);
    compositor::Surface* surface = surfaceFromResource(surfaceResource);
    if (!surface) {
        wl_resource_post_error(resource, CUTEFISH_SHELL_V1_ERROR_ROLE,
                               "the surface is already gone");
        return;
    }
    if (shell->windowManager()->shellMetadata(surface)) {
        wl_resource_post_error(resource, CUTEFISH_SHELL_V1_ERROR_ROLE,
                               "the surface already has a cutefish_shell_surface_v1");
        return;
    }

    wl_resource* shellSurfaceResource =
        wl_resource_create(client, &cutefish_shell_surface_v1_interface,
                           wl_resource_get_version(resource), id);
    if (!shellSurfaceResource) {
        wl_client_post_no_memory(client);
        return;
    }

    auto shellSurface =
        std::make_unique<CutefishShellSurface>(shell->windowManager(), surface);
    // An unconfigured shell surface is already claimed, so a second
    // get_shell_surface on the same wl_surface is caught above.
    shell->windowManager()->setShellMetadata(surface, ShellWindowMetadata {});
    setImplementation(shellSurfaceResource, &kShellSurfaceImplementation, std::move(shellSurface));
}

void shellGetDrag(wl_client* client, wl_resource* resource, uint32_t id)
{
    auto* shell = getUserDataAs<WaylandCutefishShell>(resource);
    wl_resource* dragResource = wl_resource_create(client, &cutefish_shell_drag_v1_interface,
                                                   wl_resource_get_version(resource), id);
    if (!dragResource) {
        wl_client_post_no_memory(client);
        return;
    }
    setImplementation(dragResource, &kDragImplementation,
                      std::make_unique<CutefishShellDrag>(shell->windowManager()->seat()->drag(),
                                                          dragResource));
}

void shellSendPaste(wl_client*, wl_resource* resource)
{
    getUserDataAs<WaylandCutefishShell>(resource)->windowManager()->sendPaste();
}

const struct cutefish_shell_v1_interface kShellImplementation = {
    shellDestroy,
    shellGetShellSurface,
    shellGetDrag,
    shellSendPaste,
};

void bindShell(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto* shell = static_cast<WaylandCutefishShell*>(data);

    // Defence in depth: the global filter already hides this interface, but a
    // bind must never be served to anything but the current trusted shell.
    if (!shell->privileged()->isTrusted(client)) {
        wl_client_post_implementation_error(client, "cutefish_shell_v1 is not available");
        return;
    }

    wl_resource* resource =
        wl_resource_create(client, &cutefish_shell_v1_interface,
                           std::min(version, kCutefishShellVersion), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &kShellImplementation, shell, nullptr);
}

} // namespace

WaylandCutefishShell::WaylandCutefishShell(wm::WindowManager* windowManager,
                                           PrivilegedClients* privileged)
    : m_windowManager(windowManager)
    , m_privileged(privileged)
{
}

wl_global* createCutefishShellGlobal(wl_display* display, WaylandCutefishShell* shell)
{
    wl_global* global = wl_global_create(display, &cutefish_shell_v1_interface,
                                         kCutefishShellVersion, shell, bindShell);
    if (!global)
        return nullptr;
    shell->privileged()->addPrivateGlobal(global);
    return global;
}

} // namespace cutefish::wayland
