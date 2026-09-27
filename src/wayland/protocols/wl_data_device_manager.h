// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/wayland/wl_data_device_manager.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Clipboard selection and pointer drag-and-drop, wl_data_device_manager versions 1–3.
#pragma once

#include <cstdint>
#include <wayland-server-core.h>

namespace cutefish::wayland {

struct WaylandSeat;
constexpr uint32_t kWlDataDeviceManagerVersion = 3;
wl_global* createDataDeviceManagerGlobal(wl_display* display, WaylandSeat* seat);

} // namespace cutefish::wayland
