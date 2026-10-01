// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/window_hints.h"

#include <cutefish-window-hints-v1-server-protocol.h>

#include "compositor/surface.h"
#include "wayland/server_util.h"

namespace cutefish::wayland {
namespace {

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

const struct cutefish_window_hints_v1_interface implementation = {destroy, setSkipTaskbar,
                                                                  setAlwaysOnTop, setResizeAnchor};

void bind(wl_client *client, void *, uint32_t, uint32_t id)
{
    auto *resource = wl_resource_create(client, &cutefish_window_hints_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
}

} // namespace

wl_global *createWindowHintsGlobal(wl_display *display)
{
    return wl_global_create(display, &cutefish_window_hints_v1_interface, 1, nullptr, bind);
}

} // namespace cutefish::wayland
