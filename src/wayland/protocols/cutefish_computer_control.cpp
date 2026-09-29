// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/cutefish_computer_control.h"

#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <QPointF>
#include <QRect>

#include <cutefish-computer-control-v1-server-protocol.h>

#include "computer_control/controller.h"
#include "wayland/server_util.h"

namespace cutefish::wayland {
namespace {

using computer_control::Control;
using computer_control::Controller;
using computer_control::Rejection;
using computer_control::RevokeReason;
using computer_control::WindowInfo;

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

    void window(uint32_t handle, const WindowInfo &window) override
    {
        cutefish_computer_control_v1_send_window(
                resource, handle, window.applicationId.toUtf8().constData(),
                window.title.toUtf8().constData(), window.bounds.x(), window.bounds.y(),
                window.bounds.width(), window.bounds.height(), window.flags);
    }

    void windowsDone() override { cutefish_computer_control_v1_send_windows_done(resource); }

    void pointer(const QPointF &position) override
    {
        cutefish_computer_control_v1_send_pointer(resource, wl_fixed_from_double(position.x()),
                                                  wl_fixed_from_double(position.y()));
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

// Only a regular file is read at once, so the client cannot keep the
// compositor waiting.
constexpr off_t kClipboardLimit = 16 * 1024 * 1024;

// Returns the text, or nothing after posting the error. Closes the fd.
std::optional<std::string> readClipboard(wl_resource *resource, int fd)
{
    struct stat info {};
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size > kClipboardLimit) {
        close(fd);
        wl_resource_post_error(resource, CUTEFISH_COMPUTER_CONTROL_V1_ERROR_INVALID_CLIPBOARD,
                               "the fd must be a regular file of at most 16 MiB");
        return std::nullopt;
    }
    std::string text(static_cast<size_t>(info.st_size), '\0');
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t count = pread(fd, text.data() + done, text.size() - done,
                                    static_cast<off_t>(done));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        done += static_cast<size_t>(count);
    }
    close(fd);
    text.resize(done);
    return text;
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
        [](wl_client *, wl_resource *resource) {
            controlOf(resource)->listWindows();
        },
        [](wl_client *, wl_resource *resource, uint32_t handle) {
            controlOf(resource)->activateWindow(handle);
        },
        [](wl_client *, wl_resource *resource) {
            controlOf(resource)->queryPointer();
        },
        [](wl_client *, wl_resource *resource, uint32_t handle) {
            controlOf(resource)->closeWindow(handle);
        },
        [](wl_client *, wl_resource *resource, uint32_t handle, int32_t x, int32_t y,
           int32_t width, int32_t height) {
            controlOf(resource)->setWindowFrame(handle, QRect(x, y, width, height));
        },
        [](wl_client *, wl_resource *resource, int32_t fd) {
            if (const auto text = readClipboard(resource, fd))
                controlOf(resource)->setClipboard(*text);
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
