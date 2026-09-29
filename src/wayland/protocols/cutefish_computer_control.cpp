// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/cutefish_computer_control.h"

#include <memory>
#include <optional>
#include <utility>

#include <QPointF>

#include <cutefish-computer-control-v1-server-protocol.h>

#include "computer_control/controller.h"
#include "wayland/server_util.h"

namespace cutefish::wayland {
namespace {

using computer_control::Control;
using computer_control::Controller;
using computer_control::Rejection;
using computer_control::RevokeReason;

struct ControlResource final : computer_control::ControlClient {
    explicit ControlResource(wl_resource *resource)
        : resource(resource)
    {
    }

    void granted() override { cutefish_computer_control_v1_send_granted(resource); }

    void revoked(RevokeReason reason) override
    {
        cutefish_computer_control_v1_send_revoked(resource, static_cast<uint32_t>(reason));
    }

    void rejected(Rejection rejection) override
    {
        cutefish_computer_control_v1_send_rejected(resource, static_cast<uint32_t>(rejection));
    }

    wl_resource *const resource;
    std::unique_ptr<Control> control;
};

Control *controlOf(wl_resource *resource)
{
    return getUserDataAs<ControlResource>(resource)->control.get();
}

// Returns the state as a press, or nothing after posting the error.
std::optional<bool> pressed(wl_resource *resource, uint32_t state)
{
    if (state > CUTEFISH_COMPUTER_CONTROL_V1_STATE_PRESSED) {
        wl_resource_post_error(resource, CUTEFISH_COMPUTER_CONTROL_V1_ERROR_INVALID_STATE,
                               "state must be released or pressed");
        return std::nullopt;
    }
    return state == CUTEFISH_COMPUTER_CONTROL_V1_STATE_PRESSED;
}

void destroy(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

const struct cutefish_computer_control_v1_interface controlImplementation = {
        destroy,
        [](wl_client *, wl_resource *resource, wl_fixed_t x, wl_fixed_t y) {
            controlOf(resource)->motion(QPointF(wl_fixed_to_double(x), wl_fixed_to_double(y)));
        },
        [](wl_client *, wl_resource *resource, uint32_t button, uint32_t state) {
            if (const auto press = pressed(resource, state))
                controlOf(resource)->button(button, *press);
        },
        [](wl_client *, wl_resource *resource, int32_t horizontal, int32_t vertical) {
            controlOf(resource)->axis(horizontal, vertical);
        },
        [](wl_client *, wl_resource *resource, uint32_t key, uint32_t state) {
            if (const auto press = pressed(resource, state))
                controlOf(resource)->key(key, *press);
        },
        [](wl_client *, wl_resource *resource, uint32_t keysym, uint32_t state) {
            if (const auto press = pressed(resource, state))
                controlOf(resource)->keysym(keysym, *press);
        },
        [](wl_client *, wl_resource *resource, const char *text) {
            controlOf(resource)->typeText(text);
        },
};

void requestControl(wl_client *client, wl_resource *manager, uint32_t id, const char *agentName)
{
    auto *controller = getUserDataAs<Controller>(manager);
    wl_resource *resource = wl_resource_create(client, &cutefish_computer_control_v1_interface,
                                               wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    auto *control = new ControlResource(resource);
    wl_resource_set_implementation(resource, &controlImplementation, control,
                                   [](wl_resource *resource) {
                                       delete getUserDataAs<ControlResource>(resource);
                                   });
    control->control = controller->request(control, client, QString::fromUtf8(agentName));
}

const struct cutefish_computer_control_manager_v1_interface managerImplementation = {
        destroy,
        requestControl,
};

} // namespace

WaylandComputerControl::WaylandComputerControl(wl_display *display, Controller *controller,
                                               std::function<bool(const wl_client *)> authorize)
    : m_controller(controller)
    , m_authorize(std::move(authorize))
{
    m_global = wl_global_create(display, &cutefish_computer_control_manager_v1_interface, 1, this,
                                bind);
}

WaylandComputerControl::~WaylandComputerControl()
{
    if (m_global)
        wl_global_destroy(m_global);
}

void WaylandComputerControl::bind(wl_client *client, void *data, uint32_t version, uint32_t id)
{
    auto *self = static_cast<WaylandComputerControl *>(data);
    if (!self->m_authorize(client)) {
        wl_client_post_implementation_error(client, "Computer control not authorized");
        return;
    }
    wl_resource *resource =
            wl_resource_create(client, &cutefish_computer_control_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &managerImplementation, self->m_controller, nullptr);
}

} // namespace cutefish::wayland
