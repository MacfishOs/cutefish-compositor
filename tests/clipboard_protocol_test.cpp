// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <signal.h>

#include <QDir>

#include "clipboard_test_util.h"

using namespace clipboard_test;

TEST(ClipboardProtocol, RealBytesOpaqueFormatsRepeatedReceivesAndIdleProgress)
{
    Harness h;
    auto& a = h.addClient();
    auto& b = h.addClient();
    const std::map<std::string, std::string> data = {
        {"text/plain;charset=utf-8", "Unicode \xf0\x9f\x90\x9f \xe4\xb8\xad"},
        {"text/plain", "plain"},
        {"text/html", "<b>hello</b>"},
        {"text/uri-list", "file:///generated-not-opened\r\n"},
        {"image/png", std::string("\x89PNG\0\xff", 6)},
        {"application/x-cutefish-test", std::string(256 * 1024, '\xab')}};
    h.publish(a, data);
    h.focus(b);
    ASSERT_NE(nullptr, b.selection);
    EXPECT_EQ(data.size(), b.formats[b.selection].size());
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& [mime, bytes] : data)
            EXPECT_EQ(bytes, h.receive(b, b.selection, mime));
    EXPECT_EQ(12u, a.sends);
    EXPECT_EQ("", h.receive(b, b.selection, "unadvertised"));
    EXPECT_EQ(12u, a.sends);
    // No renderer, output submit or frame callbacks exist in this harness.
}

TEST(ClipboardProtocol, FocusLateDevicesOrderingMultipleDevicesAndFocusReturn)
{
    Harness h;
    auto& a = h.addClient();
    auto& b = h.addClient(false);
    h.publish(a, {{"text/plain", "selection"}});
    h.focus(b);
    b.events.clear();
    b.addDevice();
    h.sync(b);
    EXPECT_EQ((std::vector<std::string> {"offer", "mime", "selection"}), b.events);
    auto* old = b.selection;
    b.addDevice();
    h.sync(b);
    EXPECT_EQ(2u, b.offers.size());
    EXPECT_EQ("selection", h.receive(b, old, "text/plain"));
    auto* secondSurface = wl_compositor_create_surface(b.compositor);
    h.sync(b);
    const auto count = b.offers.size();
    auto* resource = wl_client_get_object(
        b.serverClient, wl_proxy_get_id(reinterpret_cast<wl_proxy*>(secondSurface)));
    h.seat.setFocusedSurface(wayland::getUserDataAs<compositor::Surface>(resource));
    h.sync(b);
    EXPECT_EQ(count, b.offers.size());
    h.seat.setFocusedSurface(nullptr);
    h.sync(b);
    EXPECT_EQ("", h.receive(b, old, "text/plain"));
    EXPECT_TRUE(h.seat.selection()->source());
    b.events.clear();
    h.seat.setFocusedSurface(wayland::getUserDataAs<compositor::Surface>(resource));
    h.sync(b);
    EXPECT_EQ(count + 2, b.offers.size());
    EXPECT_EQ("offer", b.events.front());
    EXPECT_EQ("enter", b.events.back());
    EXPECT_EQ("", h.receive(b, old, "text/plain"));
    EXPECT_EQ("selection", h.receive(b, b.selection, "text/plain"));
    wl_surface_destroy(secondSurface);
}

TEST(ClipboardProtocol, ReplacementClearEmptyPayloadDestructionAndStaleOffers)
{
    Harness h;
    auto& a = h.addClient();
    auto& b = h.addClient();
    h.publish(a, {{"text/plain", "first"}});
    h.focus(b);
    auto* old = b.selection;
    h.publish(a, {{"text/plain", ""}});
    EXPECT_EQ(1u, a.cancelled);
    h.focus(b);
    ASSERT_TRUE(b.selection);
    EXPECT_EQ("", h.receive(b, b.selection, "text/plain"));
    EXPECT_EQ("", h.receive(b, old, "text/plain"));
    wl_data_source_destroy(a.sources.back());
    a.sources.pop_back();
    h.sync(a);
    h.sync(b);
    EXPECT_EQ(nullptr, b.selection);
    EXPECT_FALSE(h.seat.selection()->source());
    h.publish(a, {{"text/plain", "third"}});
    h.focus(b);
    h.focus(a);
    wl_data_device_set_selection(a.devices[0], nullptr, a.serial);
    h.sync(a);
    EXPECT_FALSE(h.seat.selection()->source());
    EXPECT_EQ(2u, a.cancelled);
}

TEST(ClipboardProtocol, SerialPolicyDoesNotAuthorizeOtherClientsOrOldFocus)
{
    Harness h;
    auto& a = h.addClient();
    auto& b = h.addClient();
    h.focus(a);
    const auto oldSerial = a.serial;
    auto* source = a.source({{"text/plain", "a"}});
    h.sync(a);
    h.focus(b);
    wl_data_device_set_selection(a.devices[0], source, b.serial);
    h.sync(a);
    EXPECT_FALSE(h.seat.selection()->source());
    h.focus(a);
    wl_data_device_set_selection(a.devices[0], source, oldSerial);
    h.sync(a);
    EXPECT_FALSE(h.seat.selection()->source());
    wl_data_device_set_selection(a.devices[0], source, a.serial);
    h.sync(a);
    EXPECT_TRUE(h.seat.selection()->source());
    const auto enter = a.serial;
    h.seat.onKey(input::Timestamp {}, 46, true);
    h.sync(a);
    auto* second = a.source({{"text/plain", "b"}});
    wl_data_device_set_selection(a.devices[0], second, a.serial);
    h.sync(a);
    const auto generation = h.seat.selection()->generation();
    wl_data_device_set_selection(a.devices[0], nullptr, enter);
    h.sync(a);
    EXPECT_EQ(generation, h.seat.selection()->generation());
    h.seat.onKey(input::Timestamp {}, 46, false);
    h.sync(a);
    wl_data_device_set_selection(a.devices[0], nullptr, a.serial);
    h.sync(a);
    EXPECT_FALSE(h.seat.selection()->source());
}

TEST(ClipboardProtocol, DeviceDestructionDoesNotClearOwnerButInvalidatesItsOffers)
{
    Harness h;
    auto& a = h.addClient();
    auto& b = h.addClient();
    h.publish(a, {{"text/plain", "bytes"}});
    h.focus(b);
    auto* old = b.selection;
    wl_data_device_release(b.devices.back());
    b.devices.pop_back();
    h.sync(b);
    EXPECT_EQ("", h.receive(b, old, "text/plain"));
    wl_data_device_release(a.devices.back());
    a.devices.pop_back();
    h.sync(a);
    EXPECT_TRUE(h.seat.selection()->source());
    b.addDevice();
    h.sync(b);
    EXPECT_EQ("bytes", h.receive(b, b.selection, "text/plain"));
    h.clients.erase(h.clients.begin());
    h.sync(b);
    EXPECT_EQ(nullptr, b.selection);
    EXPECT_FALSE(h.seat.selection()->source());
}

TEST(ClipboardProtocol, SlowProducerAndBrokenPipeDoNotBlockServer)
{
    Harness h;
    auto& a = h.addClient();
    auto& b = h.addClient();
    h.publish(a, {{"text/plain", std::string(131072, 'x')}});
    h.focus(b);
    a.stall = true;
    int fds[2];
    ASSERT_EQ(0, pipe2(fds, O_NONBLOCK | O_CLOEXEC));
    wl_data_offer_receive(b.selection, "text/plain", fds[1]);
    close(fds[1]);
    h.sync(b);
    ASSERT_TRUE(h.until([&] {
        return a.sends == 1;
    }));
    h.sync(a);
    h.sync(b); // Stalled peer cannot stall control messages.
    close(fds[0]);
    a.stall = false;
    ASSERT_TRUE(h.until([&] {
        return a.writes.empty();
    }));
}

TEST(ClipboardProtocol, OlderVersionsTransferSelectionWithoutDndEvents)
{
    for (uint32_t version : {1u, 2u}) {
        Harness h;
        auto& a = h.addClient(true, version);
        auto& b = h.addClient(true, version);
        h.publish(a, {{"text/plain", "older client"}});
        h.focus(b);
        ASSERT_TRUE(b.selection);
        EXPECT_EQ("older client", h.receive(b, b.selection, "text/plain"));
    }
}

TEST(ClipboardProtocol, RetiredSourceReuseAndDndSelectionMisuseAreProtocolErrors)
{
    for (int kind = 0; kind < 5; ++kind) {
        Harness h;
        auto& a = h.addClient();
        h.publish(a, {{"text/plain", "old"}});
        if (kind == 0) {
            auto* old = a.sources.front();
            h.publish(a, {{"text/plain", "new"}});
            wl_data_device_set_selection(a.devices.front(), old, a.serial);
        } else if (kind == 1) {
            wl_data_offer_finish(a.selection);
        } else if (kind == 2) {
            wl_data_offer_set_actions(a.selection, 1, 1);
        } else if (kind == 3) {
            auto* source = a.source({{"text/plain", "drag"}});
            wl_data_source_set_actions(source, 1);
            wl_data_device_set_selection(a.devices.front(), source, a.serial);
        } else {
            auto* source = a.source({{"text/plain", "drag"}});
            wl_data_source_set_actions(source, 8);
        }
        a.expectError = true;
        ASSERT_TRUE(h.until([&] {
            return wl_display_get_error(a.display) != 0;
        }));
        EXPECT_EQ(EPROTO, wl_display_get_error(a.display));
        const wl_interface* interface = nullptr;
        uint32_t id;
        const auto error = wl_display_get_protocol_error(a.display, &interface, &id);
        EXPECT_EQ(kind == 0       ? WL_DATA_DEVICE_ERROR_USED_SOURCE
                      : kind == 1 ? WL_DATA_OFFER_ERROR_INVALID_FINISH
                      : kind == 2 ? WL_DATA_OFFER_ERROR_INVALID_OFFER
                      : kind == 3 ? WL_DATA_SOURCE_ERROR_INVALID_SOURCE
                                  : WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                  error);
    }
}

TEST(ClipboardProtocol, DragWithoutPointerPressCancelsWithoutChangingSelection)
{
    Harness h;
    auto& a = h.addClient();
    h.focus(a);
    const auto count = h.compositor.surfaces().size();
    auto* source = a.source({{"text/plain", "drag"}});
    wl_data_source_set_actions(source, 1);
    wl_data_device_start_drag(a.devices.front(), source, a.surface, nullptr, a.serial);
    h.sync(a);
    EXPECT_EQ(1u, a.cancelled);
    EXPECT_FALSE(h.seat.selection()->source());
    EXPECT_EQ(count, h.compositor.surfaces().size());
    wl_data_device_start_drag(a.devices.front(), nullptr, a.surface, nullptr, a.serial);
    h.sync(a);
    EXPECT_FALSE(h.seat.selection()->source());
}

TEST(ClipboardProtocol, AnotherSeatCannotPublishOrReceiveThisSeatsSelection)
{
    Harness h;
    input::Seat anotherSeat(&h.compositor);
    wayland::WaylandSeat anotherAdapter(&anotherSeat, h.server.serialTracker());
    wayland::createSeatGlobal(h.server.display(), &anotherAdapter);
    auto& a = h.addClient(); // Registry binds the last seat, which belongs to another controller.
    h.focus(a);
    wl_data_device_set_selection(a.devices.front(), a.source({{"text/plain", "wrong seat"}}),
                                 a.serial);
    h.sync(a);
    EXPECT_FALSE(h.seat.selection()->source());
    EXPECT_EQ(nullptr, a.selection);
    h.clients.clear();
    wl_display_destroy_clients(h.server.display());
}

TEST(ClipboardProtocol, RecipientDisconnectDuringForwardedTransferDoesNotRetainFds)
{
    const auto before =
        QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
    {
        Harness h;
        auto& a = h.addClient();
        auto& b = h.addClient();
        h.publish(a, {{"text/plain", std::string(131072, 'x')}});
        h.focus(b);
        a.stall = true;
        int fds[2];
        ASSERT_EQ(0, pipe2(fds, O_NONBLOCK | O_CLOEXEC));
        wl_data_offer_receive(b.selection, "text/plain", fds[1]);
        close(fds[1]);
        h.sync(b);
        ASSERT_TRUE(h.until([&] {
            return a.sends == 1;
        }));
        h.clients.pop_back();
        close(fds[0]);
        a.stall = false;
        ASSERT_TRUE(h.until([&] {
            return a.writes.empty();
        }));
        EXPECT_TRUE(h.seat.selection()->source());
    }
    EXPECT_EQ(before,
              QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
