// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cutefish-shell-v1-client-protocol.h>
#include <linux/input-event-codes.h>

#include "clipboard_test_util.h"
#include "cursor/cursor.h"
#include "input/input_manager.h"
#include "scene/surface_node.h"
#include "shm_test_util.h"
#include "wayland/privileged_client.h"
#include "wayland/protocols/cutefish_shell.h"
#include "wm/window_manager.h"

using namespace clipboard_test;

namespace {

compositor::Surface *map(Harness &h, Client &c, int x)
{
    auto *resource = wl_client_get_object(c.serverClient,
                                          wl_proxy_get_id(reinterpret_cast<wl_proxy *>(c.surface)));
    auto *surface = wayland::surfaceFromResource(resource);
    auto pool = testsupport::makePool(100 * 100 * 4);
    auto buffer =
            pool->createBuffer(QSize(100, 100), 0, 400, render::PixelFormat::Bgra8888, nullptr);
    surface->attach(buffer, {});
    surface->commit();
    surface->setPosition({x, 0});
    return surface;
}

wl_data_source *start(Harness &h, Client &a, uint32_t actions = 3, bool nullSource = false)
{
    h.focus(a);
    h.seat.onPointerMotion({}, {10, 10});
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, true);
    h.sync(a);
    auto *source = nullSource ? nullptr
                              : a.source({{"text/plain", "drag bytes"},
                                          {"application/x-test", std::string(131072, 'x')}});
    if (source && a.version >= 3)
        wl_data_source_set_actions(source, actions);
    wl_data_device_start_drag(a.devices.front(), source, a.surface, nullptr, a.buttonSerial);
    h.sync(a);
    EXPECT_TRUE(h.seat.drag()->active());
    return source;
}

void accept(Harness &h, Client &b, uint32_t actions = 3, uint32_t preferred = 2)
{
    ASSERT_NE(nullptr, b.dragOffer);
    wl_data_offer_accept(b.dragOffer, b.dragSerial, "text/plain");
    if (b.version >= 3)
        wl_data_offer_set_actions(b.dragOffer, actions, preferred);
    h.sync(b);
}

// A cutefish_shell_drag_v1 and what it heard, in order.
struct ShellDrag {
    cutefish_shell_drag_v1 *object = nullptr;
    Client *client = nullptr;
    std::vector<std::string> events;
    QPointF location;
    // How many cancels the client's data source had seen when the drop came.
    size_t cancelsAtDrop = 0;

    ShellDrag(Harness &h, Client &c)
        : client(&c)
    {
        cutefish_shell_v1 *shell = nullptr;
        auto *registry = wl_display_get_registry(c.display);
        static const wl_registry_listener registryListener = {
            [](void *data, wl_registry *registry, uint32_t name, const char *interface,
               uint32_t) {
                if (std::string(interface) == "cutefish_shell_v1")
                    *static_cast<cutefish_shell_v1 **>(data) = static_cast<cutefish_shell_v1 *>(
                            wl_registry_bind(registry, name, &cutefish_shell_v1_interface, 1));
            },
            [](void *, wl_registry *, uint32_t) {}};
        wl_registry_add_listener(registry, &registryListener, &shell);
        h.sync(c);
        EXPECT_NE(nullptr, shell);
        object = cutefish_shell_v1_get_drag(shell);
        static const cutefish_shell_drag_v1_listener listener = {
            [](void *data, cutefish_shell_drag_v1 *, wl_fixed_t x, wl_fixed_t y) {
                auto *drag = static_cast<ShellDrag *>(data);
                drag->events.push_back("motion");
                drag->location = QPointF(wl_fixed_to_double(x), wl_fixed_to_double(y));
            },
            [](void *data, cutefish_shell_drag_v1 *, wl_fixed_t x, wl_fixed_t y) {
                auto *drag = static_cast<ShellDrag *>(data);
                drag->events.push_back("dropped");
                drag->location = QPointF(wl_fixed_to_double(x), wl_fixed_to_double(y));
                drag->cancelsAtDrop = drag->client->cancelled;
            },
            [](void *data, cutefish_shell_drag_v1 *) {
                static_cast<ShellDrag *>(data)->events.push_back("cancelled");
            }};
        cutefish_shell_drag_v1_add_listener(object, &listener, this);
        cutefish_shell_v1_destroy(shell);
        wl_registry_destroy(registry);
        h.sync(c);
    }

    ~ShellDrag() { cutefish_shell_drag_v1_destroy(object); }
};

// The trusted shell, with cutefish_shell_v1.
struct ShellHarness {
    Harness h;
    wayland::PrivilegedClients privileged {h.server.display()};
    wm::WindowManager windowManager {&h.compositor, &h.seat};
    wayland::WaylandCutefishShell shell {&windowManager, &privileged};

    ShellHarness() { wayland::createCutefishShellGlobal(h.server.display(), &shell); }

    // The trusted connection goes before the registry that knows it.
    ~ShellHarness()
    {
        h.clients.clear();
        wl_display_destroy_clients(h.server.display());
    }

    Client &addShell()
    {
        return h.addClient(true, 3, [this](int fd) {
            return privileged.adoptShellConnection(fd);
        });
    }
};

} // namespace

TEST(DndProtocol, CrossClientDropRetainsOfferUntilFinishAndDoesNotChangeClipboard)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    map(h, a, 0);
    map(h, b, 150);
    h.publish(a, {{"text/plain", "clipboard"}});
    const auto generation = h.seat.selection()->generation();
    start(h, a);
    auto *old = a.dragOffer;
    h.seat.onPointerMotion({}, {160, 10});
    h.sync(b);
    EXPECT_EQ("", h.receive(a, old, "text/plain"));
    accept(h, b);
    h.sync(a);
    EXPECT_EQ(2u, b.dragAction);
    EXPECT_EQ(2u, a.sourceAction);
    EXPECT_EQ("drag bytes", h.receive(b, b.dragOffer, "text/plain"));
    h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
    h.sync(b);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_EQ(1u, b.drops);
    EXPECT_EQ(1u, a.performed);
    EXPECT_EQ(0u, a.finished);
    EXPECT_EQ(std::string(131072, 'x'), h.receive(b, b.dragOffer, "application/x-test"));
    wl_data_offer_finish(b.dragOffer);
    h.sync(b);
    h.sync(a);
    EXPECT_EQ(1u, a.finished);
    EXPECT_EQ(0u, a.cancelled);
    EXPECT_EQ(generation, h.seat.selection()->generation());
}

TEST(DndProtocol, ViewportDestinationKeepsDragCoordinatesSurfaceLocal)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    map(h, a, 0);
    auto *target = map(h, b, 150);
    target->setCrop(QRectF(25, 10, 50, 50));
    target->setViewport(QSize(200, 100));
    target->commit();
    start(h, a);
    h.seat.onPointerMotion({}, {250, 50});
    h.sync(b);
    EXPECT_EQ(b.dragEnterPosition, QPointF(100, 50));
    h.seat.onPointerMotion({}, {275.5, 60.25});
    h.sync(b);
    EXPECT_EQ(b.dragMotionPosition, QPointF(125.5, 60.25));
    h.seat.onKey({}, KEY_ESC, true);
}

TEST(DndProtocol, LegacyClientsFinishOnOfferDestruction)
{
    for (uint32_t version : {1u, 2u}) {
        Harness h;
        auto &a = h.addClient();
        auto &b = h.addClient(true, version);
        map(h, a, 0);
        map(h, b, 150);
        start(h, a);
        h.seat.onPointerMotion({}, {160, 10});
        h.sync(b);
        accept(h, b);
        h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
        h.sync(b);
        EXPECT_EQ(1u, b.drops);
        EXPECT_EQ("drag bytes", h.receive(b, b.dragOffer, "text/plain"));
        wl_data_offer_destroy(b.dragOffer);
        std::erase(b.offers, b.dragOffer);
        h.sync(b);
        h.sync(a);
        EXPECT_EQ(1u, a.finished);
        EXPECT_EQ(0u, a.cancelled);
    }
}

TEST(DndProtocol, EscapeRejectedActionsAndDestroyedOfferCancel)
{
    for (int kind = 0; kind < 4; ++kind) {
        Harness h;
        auto &a = h.addClient();
        auto &b = h.addClient();
        map(h, a, 0);
        map(h, b, 150);
        start(h, a, 1);
        h.seat.onPointerMotion({}, {160, 10});
        h.sync(b);
        accept(h, b, kind == 1 ? 2 : 1, kind == 1 ? 2 : 1);
        if (kind == 0) {
            h.seat.onKey({}, KEY_ESC, true);
            h.seat.onKey({}, KEY_ESC, false);
        } else if (kind == 2) {
            wl_data_offer_destroy(b.dragOffer);
            std::erase(b.offers, b.dragOffer);
            h.sync(b);
        } else if (kind == 3) {
            wl_surface_destroy(b.surface);
            b.surface = nullptr;
            h.sync(b);
        }
        h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
        h.sync(a);
        h.sync(b);
        EXPECT_FALSE(h.seat.drag()->active());
        EXPECT_EQ(1u, a.cancelled);
        EXPECT_EQ(0u, b.drops);
        EXPECT_EQ(input::NoButton, h.seat.pressedButtons());
    }
}

TEST(DndProtocol, NullSourceIsRestrictedToOriginClient)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    map(h, a, 0);
    map(h, b, 150);
    start(h, a, 0, true);
    EXPECT_NE(a.events.end(), std::find(a.events.begin(), a.events.end(), "drag-enter"));
    EXPECT_EQ(nullptr, a.dragOffer);
    h.seat.onPointerMotion({}, {160, 10});
    h.sync(b);
    EXPECT_EQ(b.events.end(), std::find(b.events.begin(), b.events.end(), "drag-enter"));
    h.seat.onPointerMotion({}, {10, 10});
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, false);
    h.sync(a);
    EXPECT_EQ(1u, a.drops);
    EXPECT_EQ(0u, b.drops);
}

TEST(DndProtocol, ReleasedAndOtherClientsSerialsCannotStartDrag)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    map(h, a, 0);
    map(h, b, 150);
    start(h, a);
    const auto oldSerial = a.buttonSerial;
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, false);
    h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, true);
    h.sync(b);
    auto *source = a.source({{"text/plain", "invalid"}});
    wl_data_device_start_drag(a.devices.front(), source, a.surface, nullptr, b.buttonSerial);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, true);
    source = a.source({{"text/plain", "stale"}});
    wl_data_device_start_drag(a.devices.front(), source, a.surface, nullptr, oldSerial);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
}

TEST(DndProtocol, AskResolvesAfterDropAndModifiersSelectAmongSharedActions)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    map(h, a, 0);
    map(h, b, 150);
    start(h, a, 7);
    h.seat.onPointerMotion({}, {160, 10});
    h.sync(b);
    accept(h, b, 7, 2);
    h.seat.onKey({}, KEY_LEFTCTRL, true);
    h.sync(b);
    EXPECT_EQ(1u, b.dragAction);
    h.seat.onKey({}, KEY_LEFTCTRL, false);
    h.sync(b);
    EXPECT_EQ(2u, b.dragAction);
    wl_data_offer_set_actions(b.dragOffer, 7, 4);
    h.sync(b);
    h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
    h.sync(b);
    EXPECT_EQ(1u, b.drops);
    EXPECT_EQ(4u, b.dragAction);
    wl_data_offer_set_actions(b.dragOffer, 3, 2);
    wl_data_offer_finish(b.dragOffer);
    h.sync(b);
    h.sync(a);
    EXPECT_EQ(2u, a.sourceAction);
    EXPECT_EQ(1u, a.finished);
    EXPECT_EQ(0u, a.cancelled);
}

TEST(DndProtocol, SourceOriginAndDestinationDeviceDestructionUnwindTransfers)
{
    for (int kind = 0; kind < 4; ++kind) {
        Harness h;
        auto &a = h.addClient();
        auto &b = h.addClient();
        map(h, a, 0);
        map(h, b, 150);
        auto *source = start(h, a);
        h.seat.onPointerMotion({}, {160, 10});
        h.sync(b);
        accept(h, b);
        if (kind == 3)
            h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
        if (kind == 0) {
            wl_data_source_destroy(source);
            std::erase(a.sources, source);
            h.sync(a);
        } else if (kind == 1) {
            wl_surface_destroy(a.surface);
            a.surface = nullptr;
            h.sync(a);
        } else {
            wl_data_device_release(b.devices.back());
            b.devices.pop_back();
            h.sync(b);
        }
        h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
        h.sync(a);
        EXPECT_FALSE(h.seat.drag()->active());
        EXPECT_EQ(kind == 0 ? 0u : 1u, a.cancelled);
        EXPECT_EQ("", h.receive(b, b.dragOffer, "text/plain"));
        EXPECT_FALSE(h.seat.cursorOverride());
    }
}

TEST(DndProtocol, DragIconTracksCommittedOffsetAndDetachesOnCancellation)
{
    Harness h;
    scene::Node icons(QStringLiteral("icons"));
    h.seat.setDragIconContainer(&icons);
    scene::Node cursors(QStringLiteral("cursors"));
    auto *clientCursors =
            cursors.addChild(std::make_unique<scene::Node>(QStringLiteral("client-cursors")));
    h.seat.setCursorContainer(clientCursors);
    cursor::Cursor cursor(h.seat, cursors);
    auto &a = h.addClient();
    map(h, a, 0);
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, true);
    h.sync(a);
    auto *icon = wl_compositor_create_surface(a.compositor);
    auto *source = a.source({{"text/plain", "icon"}});
    wl_data_source_set_actions(source, 1);
    wl_data_device_start_drag(a.devices.front(), source, a.surface, icon, a.buttonSerial);
    h.sync(a);
    auto *surface = wayland::surfaceFromResource(wl_client_get_object(
            a.serverClient, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(icon))));
    ASSERT_TRUE(surface->hasRole());
    EXPECT_EQ(&icons, surface->node()->parent());
    auto pool = testsupport::makePool(32 * 32 * 4);
    auto buffer = pool->createBuffer({32, 32}, 0, 128, render::PixelFormat::Bgra8888, nullptr);
    surface->attach(buffer, {-7, -9});
    surface->commit();
    EXPECT_FALSE(clientCursors->isEffectivelyVisible());
    EXPECT_TRUE(surface->node()->isEffectivelyVisible());
    EXPECT_EQ(QPoint(3, 1), surface->position());
    h.seat.onPointerMotion({}, {70, 60});
    EXPECT_EQ(QPoint(63, 51), surface->position());
    EXPECT_EQ(h.seat.drag()->origin(), h.seat.drag()->target());
    h.seat.onKey({}, KEY_ESC, true);
    EXPECT_EQ(nullptr, surface->node()->parent());
    EXPECT_FALSE(surface->hasRole());
    EXPECT_FALSE(h.seat.cursorOverride());
    wl_surface_destroy(icon);
    h.seat.setDragIconContainer(nullptr);
    h.seat.setCursorContainer(nullptr);
}

TEST(DndProtocol, ViewportDragIconPreservesOffsetsAndSampling)
{
    Harness h;
    scene::Node icons(QStringLiteral("icons"));
    h.seat.setDragIconContainer(&icons);
    scene::Node cursors(QStringLiteral("cursors"));
    auto *clientCursors =
            cursors.addChild(std::make_unique<scene::Node>(QStringLiteral("client-cursors")));
    h.seat.setCursorContainer(clientCursors);
    cursor::Cursor cursor(h.seat, cursors);
    auto &a = h.addClient();
    map(h, a, 0);
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, true);
    h.sync(a);
    auto *icon = wl_compositor_create_surface(a.compositor);
    auto *source = a.source({{"text/plain", "icon"}});
    wl_data_source_set_actions(source, 1);
    wl_data_device_start_drag(a.devices.front(), source, a.surface, icon, a.buttonSerial);
    h.sync(a);
    auto *surface = wayland::surfaceFromResource(wl_client_get_object(
            a.serverClient, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(icon))));
    ASSERT_TRUE(surface->hasRole());
    EXPECT_EQ(&icons, surface->node()->parent());
    auto pool = testsupport::makePool(32 * 32 * 4);
    auto buffer = pool->createBuffer({32, 32}, 0, 128, render::PixelFormat::Bgra8888, nullptr);
    surface->attach(buffer, {-7, -9});
    surface->setCrop(QRectF(4, 8, 16, 16));
    surface->setViewport(QSize(48, 24));
    surface->commit();
    EXPECT_EQ(surface->contentSize(), QSize(48, 24));
    EXPECT_EQ(surface->surfaceToBuffer().map(QPointF()), QPointF(4, 8));
    EXPECT_FALSE(clientCursors->isEffectivelyVisible());
    EXPECT_TRUE(surface->node()->isEffectivelyVisible());
    EXPECT_EQ(QPoint(3, 1), surface->position());
    h.seat.onPointerMotion({}, {70, 60});
    EXPECT_EQ(QPoint(63, 51), surface->position());
    EXPECT_EQ(h.seat.drag()->origin(), h.seat.drag()->target());
    h.seat.onKey({}, KEY_ESC, true);
    EXPECT_EQ(nullptr, surface->node()->parent());
    EXPECT_FALSE(surface->hasRole());
    EXPECT_FALSE(h.seat.cursorOverride());
    wl_surface_destroy(icon);
    h.seat.setDragIconContainer(nullptr);
    h.seat.setCursorContainer(nullptr);
}

TEST(DndProtocol, FinishBeforeDropAndRequestsAfterFinishAreErrors)
{
    for (bool before : {true, false}) {
        Harness h;
        auto &a = h.addClient();
        auto &b = h.addClient();
        map(h, a, 0);
        map(h, b, 150);
        start(h, a);
        h.seat.onPointerMotion({}, {160, 10});
        h.sync(b);
        accept(h, b);
        if (!before) {
            h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
            h.sync(b);
            wl_data_offer_finish(b.dragOffer);
            h.sync(b);
            wl_data_offer_accept(b.dragOffer, b.dragSerial, "text/plain");
        } else {
            wl_data_offer_finish(b.dragOffer);
        }
        b.expectError = true;
        ASSERT_TRUE(h.until([&] {
            return wl_display_get_error(b.display) != 0;
        }));
        const wl_interface *interface = nullptr;
        uint32_t id;
        EXPECT_EQ(before ? WL_DATA_OFFER_ERROR_INVALID_FINISH : WL_DATA_OFFER_ERROR_INVALID_OFFER,
                  wl_display_get_protocol_error(b.display, &interface, &id));
    }
}

TEST(DndProtocol, KeysHeldBeforeDragReceiveTheirReleaseButDragKeysStaySuppressed)
{
    for (bool throughManager : {true, false}) {
        Harness h;
        auto &a = h.addClient();
        map(h, a, 0);
        h.focus(a);
        input::InputManager manager(&h.seat);
        const auto key = [&](uint32_t code, bool pressed) {
            if (throughManager)
                manager.onKey({}, code, pressed);
            else
                h.seat.onKey({}, code, pressed);
        };
        key(KEY_LEFTSHIFT, true);
        h.sync(a);
        start(h, a);
        key(KEY_LEFTSHIFT, false);
        key(KEY_LEFTALT, true);
        key(KEY_LEFTALT, false);
        key(KEY_ESC, true);
        key(KEY_ESC, false);
        h.sync(a);
        EXPECT_EQ((std::vector<std::pair<uint32_t, bool>> {{KEY_LEFTSHIFT, true},
                                                           {KEY_LEFTSHIFT, false}}),
                  a.keyEvents);
        EXPECT_FALSE(h.seat.drag()->active());
    }
}

TEST(DndProtocol, InertSecondDeviceDoesNotOverrideAcceptedOfferOnModifierChange)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    b.addDevice();
    h.sync(b);
    map(h, a, 0);
    map(h, b, 150);
    start(h, a);
    const auto before = b.offers.size();
    h.seat.onPointerMotion({}, {160, 10});
    h.sync(b);
    ASSERT_EQ(before + 2, b.offers.size());
    auto *first = b.offers[before];
    wl_data_offer_accept(first, b.dragSerial, "text/plain");
    wl_data_offer_set_actions(first, 3, 2);
    h.sync(b);
    h.seat.onKey({}, KEY_LEFTCTRL, true);
    h.sync(a);
    EXPECT_EQ(1u, a.sourceAction);
    h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
    h.sync(b);
    EXPECT_EQ(1u, b.drops);
    EXPECT_EQ("drag bytes", h.receive(b, first, "text/plain"));
    wl_data_offer_finish(first);
    h.sync(b);
    h.sync(a);
    EXPECT_EQ(1u, a.finished);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(DndProtocol, ATouchDragFollowsTheFingerAndDropsWhereItIsLifted)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    map(h, a, 0);
    map(h, b, 150);
    a.getTouch();
    h.sync(a);
    h.seat.onTouchDown({}, 1, {10, 10});
    h.seat.onTouchFrame();
    h.sync(a);
    ASSERT_NE(0u, a.touchSerial);

    auto *source = a.source({{"text/plain", "drag bytes"}});
    wl_data_source_set_actions(source, 3);
    wl_data_device_start_drag(a.devices.front(), source, a.surface, nullptr, a.touchSerial);
    h.sync(a);
    ASSERT_TRUE(h.seat.drag()->active());
    EXPECT_EQ(std::optional<int32_t>(1), h.seat.drag()->touchId());
    // The drag has the point now; the client that started it is told so.
    EXPECT_EQ(1u, a.touchCancels);

    // The pointer does not move a drag that follows a finger.
    h.seat.onPointerMotion({}, {160, 10});
    h.sync(b);
    EXPECT_EQ(nullptr, b.dragOffer);

    h.seat.onTouchMotion({}, 1, {160, 10});
    h.sync(b);
    accept(h, b);
    h.sync(a);
    h.seat.onTouchUp({}, 1);
    h.sync(b);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_EQ(1u, b.drops);
    EXPECT_EQ(1u, a.performed);
    wl_data_offer_finish(b.dragOffer);
    h.sync(b);
    h.sync(a);
    EXPECT_EQ(1u, a.finished);
}

TEST(DndProtocol, ALiftedFingerCannotStartADrag)
{
    Harness h;
    auto &a = h.addClient();
    map(h, a, 0);
    a.getTouch();
    h.sync(a);
    h.seat.onTouchDown({}, 1, {10, 10});
    h.seat.onTouchUp({}, 1);
    h.seat.onTouchFrame();
    h.sync(a);
    ASSERT_NE(0u, a.touchSerial);

    a.source({{"text/plain", "drag bytes"}});
    wl_data_device_start_drag(a.devices.front(), a.sources.back(), a.surface, nullptr,
                              a.touchSerial);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_EQ(1u, a.cancelled);
}

// Exo's DataDeviceManagerTest.StartDragInvalidIcon.
TEST(DndProtocol, AnIconThatAlreadyHasARoleIsAProtocolError)
{
    Harness h;
    auto &a = h.addClient();
    map(h, a, 0);
    h.focus(a);
    h.seat.onPointerMotion({}, {10, 10});
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, true);
    h.sync(a);
    auto *icon = wl_compositor_create_surface(a.compositor);
    h.sync(a);
    wayland::surfaceFromResource(
            wl_client_get_object(a.serverClient, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(icon))))
            ->setHasRole(true);
    auto *source = a.source({{"text/plain", "icon"}});
    wl_data_device_start_drag(a.devices.front(), source, a.surface, icon, a.buttonSerial);
    a.expectError = true;
    ASSERT_TRUE(h.until([&] {
        return wl_display_get_error(a.display) != 0;
    }));
    const wl_interface *interface = nullptr;
    uint32_t id = 0;
    EXPECT_EQ(uint32_t(WL_DATA_DEVICE_ERROR_ROLE),
              wl_display_get_protocol_error(a.display, &interface, &id));
    ASSERT_NE(nullptr, interface);
    EXPECT_STREQ("wl_data_device", interface->name);
    EXPECT_FALSE(h.seat.drag()->active());
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, false);
    wl_surface_destroy(icon);
}

// Exo's DataDeviceManagerTest.StartDragOriginNoRootWindow: an origin that was
// never mapped cancels the source instead of starting a drag.
TEST(DndProtocol, AnUnmappedOriginCancelsTheDrag)
{
    Harness h;
    auto &a = h.addClient();
    map(h, a, 0);
    h.focus(a);
    h.seat.onPointerMotion({}, {10, 10});
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, true);
    h.sync(a);
    auto *origin = wl_compositor_create_surface(a.compositor);
    auto *source = a.source({{"text/plain", "origin"}});
    wl_data_device_start_drag(a.devices.front(), source, origin, nullptr, a.buttonSerial);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_EQ(1u, a.cancelled);
    EXPECT_FALSE(h.seat.cursorOverride());

    // The source is spent, but the press can still start a drag from the window.
    source = a.source({{"text/plain", "window"}});
    wl_data_device_start_drag(a.devices.front(), source, a.surface, nullptr, a.buttonSerial);
    h.sync(a);
    EXPECT_TRUE(h.seat.drag()->active());
    h.seat.onKey({}, KEY_ESC, true);
    h.seat.onKey({}, KEY_ESC, false);
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, false);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    wl_surface_destroy(origin);
}

// Exo's DataDeviceTest.DataOfferNotFinished: a destination that drops its
// offer after the drop without finishing it cancels the source.
TEST(DndProtocol, AnOfferDestroyedAfterTheDropWithoutFinishCancelsTheSource)
{
    Harness h;
    auto &a = h.addClient();
    auto &b = h.addClient();
    map(h, a, 0);
    map(h, b, 150);
    start(h, a);
    h.seat.onPointerMotion({}, {160, 10});
    h.sync(b);
    accept(h, b);
    h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, false);
    h.sync(b);
    ASSERT_EQ(1u, b.drops);
    wl_data_offer_destroy(b.dragOffer);
    std::erase(b.offers, b.dragOffer);
    b.dragOffer = nullptr;
    h.sync(b);
    h.sync(a);
    EXPECT_EQ(0u, a.finished);
    EXPECT_EQ(1u, a.cancelled);
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_FALSE(h.seat.cursorOverride());
}

// Exo's PointerTest.DragDropAndPointerEnterLeaveEvents: the origin loses
// pointer focus once when the drag starts and sees no pointer input while it
// runs. Exo re-enters on the next motion after Ash's nested drag loop; here the
// drop hands the pointer back at once, without the release that ended the drag.
TEST(DndProtocol, PointerFocusLeavesForTheDragAndReturnsAfterTheDrop)
{
    Harness h;
    auto &a = h.addClient();
    map(h, a, 0);
    start(h, a);
    h.sync(a);
    ASSERT_EQ(1u, a.pointerEnters);
    EXPECT_EQ(1u, a.pointerLeaves);
    const size_t buttons = a.pointerButtons;

    h.seat.onPointerMotion({}, {11, 11});
    h.sync(a);
    EXPECT_EQ(1u, a.pointerEnters);
    EXPECT_EQ(1u, a.pointerLeaves);
    h.seat.onPointerButton({}, {11, 11}, BTN_LEFT, false);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    EXPECT_EQ(2u, a.pointerEnters);
    EXPECT_EQ(buttons, a.pointerButtons);

    h.seat.onPointerMotion({}, {12, 12});
    h.seat.onPointerButton({}, {12, 12}, BTN_LEFT, true);
    h.seat.onPointerButton({}, {12, 12}, BTN_LEFT, false);
    h.sync(a);
    EXPECT_EQ(2u, a.pointerEnters);
    EXPECT_EQ(1u, a.pointerLeaves);
    EXPECT_EQ(buttons + 2, a.pointerButtons);
}

TEST(DndProtocol, TheShellFollowsItsDragDimsTheIconAndHearsTheDropFirst)
{
    ShellHarness sh;
    auto &h = sh.h;
    scene::Node icons(QStringLiteral("icons"));
    h.seat.setDragIconContainer(&icons);
    auto &a = sh.addShell();
    map(h, a, 0);
    ShellDrag drag(h, a);
    cutefish_shell_drag_v1_set_icon_opacity(drag.object, wl_fixed_from_double(0.5));

    h.focus(a);
    h.seat.onPointerMotion({}, {10, 10});
    h.seat.onPointerButton({}, {10, 10}, BTN_LEFT, true);
    h.sync(a);
    auto *icon = wl_compositor_create_surface(a.compositor);
    auto *source = a.source({{"text/plain", "icon"}});
    wl_data_source_set_actions(source, 2);
    wl_data_device_start_drag(a.devices.front(), source, a.surface, icon, a.buttonSerial);
    h.sync(a);
    ASSERT_TRUE(h.seat.drag()->active());
    auto *surface = wayland::surfaceFromResource(wl_client_get_object(
            a.serverClient, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(icon))));
    ASSERT_FALSE(drag.events.empty());
    EXPECT_EQ("motion", drag.events.back());
    EXPECT_EQ(QPointF(10, 10), drag.location);
    EXPECT_FLOAT_EQ(0.5f, surface->node()->alpha());

    // Off every surface, where nothing would tell the shell where the drag is.
    h.seat.onPointerMotion({}, {300, 250});
    h.sync(a);
    EXPECT_EQ(QPointF(300, 250), drag.location);
    // Nothing takes the drop here, and the pointer does not say so.
    EXPECT_EQ(input::CursorShape::Default, h.seat.cursorOverride());
    cutefish_shell_drag_v1_set_icon_opacity(drag.object, wl_fixed_from_double(1));
    h.sync(a);
    EXPECT_FLOAT_EQ(1.0f, surface->node()->alpha());
    cutefish_shell_drag_v1_set_icon_opacity(drag.object, wl_fixed_from_double(0.5));
    h.sync(a);

    h.seat.onPointerButton({}, {300, 250}, BTN_LEFT, false);
    h.sync(a);
    EXPECT_FALSE(h.seat.drag()->active());
    ASSERT_EQ("dropped", drag.events.back());
    EXPECT_EQ(QPointF(300, 250), drag.location);
    EXPECT_EQ(0u, drag.cancelsAtDrop);
    EXPECT_EQ(1u, a.cancelled);
    EXPECT_EQ(0, std::count(drag.events.begin(), drag.events.end(), "cancelled"));
    EXPECT_FLOAT_EQ(1.0f, surface->node()->alpha());

    // One drag only: the next one is not followed.
    const size_t heard = drag.events.size();
    start(h, a);
    h.seat.onPointerMotion({}, {20, 20});
    h.seat.onKey({}, KEY_ESC, true);
    h.seat.onKey({}, KEY_ESC, false);
    h.sync(a);
    EXPECT_EQ(heard, drag.events.size());
    wl_surface_destroy(icon);
    h.seat.setDragIconContainer(nullptr);
}

TEST(DndProtocol, TheShellIgnoresOtherClientsDragsAndHearsEscapeAsACancel)
{
    ShellHarness sh;
    auto &h = sh.h;
    auto &a = sh.addShell();
    auto &b = h.addClient();
    map(h, a, 0);
    map(h, b, 150);
    ShellDrag drag(h, a);

    h.seat.onPointerMotion({}, {160, 10});
    h.seat.onPointerButton({}, {160, 10}, BTN_LEFT, true);
    h.sync(b);
    wl_data_device_start_drag(b.devices.front(), b.source({{"text/plain", "b"}}), b.surface,
                              nullptr, b.buttonSerial);
    h.sync(b);
    ASSERT_TRUE(h.seat.drag()->active());
    h.seat.onPointerMotion({}, {170, 20});
    EXPECT_EQ(input::CursorShape::NoDrop, h.seat.cursorOverride());
    h.seat.onPointerButton({}, {170, 20}, BTN_LEFT, false);
    h.sync(a);
    EXPECT_TRUE(drag.events.empty());

    start(h, a);
    h.seat.onPointerMotion({}, {40, 300});
    h.seat.onKey({}, KEY_ESC, true);
    h.seat.onKey({}, KEY_ESC, false);
    h.sync(a);
    ASSERT_FALSE(drag.events.empty());
    EXPECT_EQ("cancelled", drag.events.back());
    EXPECT_EQ(0, std::count(drag.events.begin(), drag.events.end(), "dropped"));
    EXPECT_EQ(QPointF(40, 300), drag.location);
}
