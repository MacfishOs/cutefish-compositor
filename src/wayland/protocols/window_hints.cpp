// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/window_hints.h"

#include <algorithm>
#include <vector>

#include <cutefish-window-hints-v1-server-protocol.h>

#include "base/strings.h"
#include "compositor/surface.h"
#include "wayland/protocols/surface_blur.h"
#include "wayland/server_util.h"

namespace cutefish::wayland {
namespace {

// Every bound manager, for the events about a client's surfaces.
std::vector<wl_resource *> &managers()
{
    static std::vector<wl_resource *> resources;
    return resources;
}

void destroy(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

void setSkipTaskbar(wl_client *, wl_resource *resource, wl_resource *surface, uint32_t skip)
{
    if (skip > 1) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "skip_taskbar must be zero or one");
        return;
    }
    getUserDataAs<compositor::Surface>(surface)->setSkipTaskbar(skip != 0);
}

void setAlwaysOnTop(wl_client *, wl_resource *resource, wl_resource *surface, uint32_t onTop)
{
    if (onTop > 1) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "always_on_top must be zero or one");
        return;
    }
    getUserDataAs<compositor::Surface>(surface)->setAlwaysOnTop(onTop != 0);
}

void setResizeAnchor(wl_client *, wl_resource *resource, wl_resource *surface, uint32_t anchor)
{
    if (anchor > CUTEFISH_WINDOW_HINTS_V1_RESIZE_ANCHOR_CENTER) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "unknown resize anchor");
        return;
    }
    getUserDataAs<compositor::Surface>(surface)->setResizeAnchor(
            anchor == CUTEFISH_WINDOW_HINTS_V1_RESIZE_ANCHOR_CENTER
                    ? compositor::ResizeAnchor::Center
                    : compositor::ResizeAnchor::TopLeft);
}

void setServerResize(wl_client *, wl_resource *resource, wl_resource *surface, uint32_t enabled)
{
    if (enabled > 1) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "server_resize must be zero or one");
        return;
    }
    getUserDataAs<compositor::Surface>(surface)->setServerResize(enabled != 0);
}

void setRestoreId(wl_client *, wl_resource *, wl_resource *surface, const char *id)
{
    getUserDataAs<compositor::Surface>(surface)->setRestoreId(base::strings::sanitizedUtf8(id));
}

void setRestoreSize(wl_client *, wl_resource *resource, wl_resource *surface, int32_t width,
                    int32_t height)
{
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "restore size must not be negative");
        return;
    }
    getUserDataAs<compositor::Surface>(surface)->setRestoreSize(base::Size(width, height));
}

void setPlacementAnchor(wl_client *, wl_resource *resource, wl_resource *surface, int32_t x,
                        int32_t y, int32_t width, int32_t height, uint32_t anchor, uint32_t gravity)
{
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "placement anchor size must not be negative");
        return;
    }
    if (anchor > CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_BOTTOM_RIGHT
        || gravity > CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_BOTTOM_RIGHT) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "unknown placement edge");
        return;
    }
    static_assert(int(compositor::PlacementEdge::Top) == CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_TOP);
    static_assert(int(compositor::PlacementEdge::BottomRight)
                  == CUTEFISH_WINDOW_HINTS_V1_PLACEMENT_EDGE_BOTTOM_RIGHT);
    getUserDataAs<compositor::Surface>(surface)->setPlacementAnchor(compositor::PlacementAnchor {
            base::Rect(x, y, width, height), compositor::PlacementEdge(anchor),
            compositor::PlacementEdge(gravity)});
}

void unsetPlacementAnchor(wl_client *, wl_resource *, wl_resource *surface)
{
    getUserDataAs<compositor::Surface>(surface)->setPlacementAnchor(std::nullopt);
}

void setBodyShape(wl_client *, wl_resource *resource, wl_resource *surface, wl_array *array)
{
    ui::Path shape;
    if (!pathFromArray(array, &shape)) {
        wl_resource_post_error(resource, CUTEFISH_WINDOW_HINTS_V1_ERROR_INVALID_VALUE,
                               "malformed body shape");
        return;
    }
    getUserDataAs<compositor::Surface>(surface)->setBodyShape(shape);
}

void unsetBodyShape(wl_client *, wl_resource *, wl_resource *surface)
{
    getUserDataAs<compositor::Surface>(surface)->setBodyShape({});
}

const struct cutefish_window_hints_v1_interface implementation = {
        destroy, setSkipTaskbar, setAlwaysOnTop, setResizeAnchor,
        setServerResize, setRestoreId, setRestoreSize, setPlacementAnchor,
        unsetPlacementAnchor, setBodyShape, unsetBodyShape};

void unbind(wl_resource *resource)
{
    auto &resources = managers();
    resources.erase(std::remove(resources.begin(), resources.end(), resource), resources.end());
}

void bind(wl_client *client, void *, uint32_t, uint32_t id)
{
    auto *resource = wl_resource_create(client, &cutefish_window_hints_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &implementation, nullptr, unbind);
    managers().push_back(resource);
}

} // namespace

void sendInteractiveResize(wl_resource *surface, bool active)
{
    wl_client *client = wl_resource_get_client(surface);
    for (wl_resource *resource : managers()) {
        if (wl_resource_get_client(resource) == client)
            cutefish_window_hints_v1_send_interactive_resize(resource, surface, active ? 1 : 0);
    }
}

wl_global *createWindowHintsGlobal(wl_display *display)
{
    return wl_global_create(display, &cutefish_window_hints_v1_interface, 1, nullptr, bind);
}

} // namespace cutefish::wayland
