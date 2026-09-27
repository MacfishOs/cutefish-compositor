// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/hardware_display_controller.cc
//           (SetCursor, MoveCursor, NextCursorBuffer, DrawCursor)
//           chromium/src/ui/ozone/platform/drm/gpu/drm_dumb_buffer.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: SkBitmap becomes raw premultiplied pixels and the
// dynamic Intel cursor-size table is omitted; one buffer size is allocated from
// the device's reported maximum.

#ifndef CUTEFISH_BACKEND_DRM_DRM_CURSOR_H_
#define CUTEFISH_BACKEND_DRM_DRM_CURSOR_H_

#include <QPoint>
#include <QSize>

#include <array>
#include <cstdint>
#include <memory>

#include "backend/drm/drm_device_api.h"

namespace cutefish::backend {

// The KMS cursor plane for one CRTC.
//
// Moving the cursor here costs one ioctl and no repaint, which is the whole
// point: with the cursor composited, every pointer motion is a full frame.
class DrmCursor
{
public:
    DrmCursor(const DrmCursor&) = delete;
    DrmCursor& operator=(const DrmCursor&) = delete;
    ~DrmCursor();

    // Returns nullptr when the device has no cursor plane or its buffers
    // cannot be allocated, in which case the caller composites the cursor.
    static std::unique_ptr<DrmCursor> create(DrmDeviceApi* device, uint32_t crtcId);

    QSize maxSize() const { return m_size; }

    // Uploads `pixels`, premultiplied RGBA8888 in increasing address order.
    // Returns false when the image is larger than the plane can show; the
    // caller must then fall back to compositing it.
    bool setImage(const uint8_t* pixels, const QSize& size, int stride, const QPoint& hotspot);

    // Takes the cursor off the plane. Idempotent.
    void hide();

    // `position` is where the hotspot should land, in output pixels.
    void move(const QPoint& position);

    bool isVisible() const { return m_visible; }

    QPoint hotspot() const { return m_hotspot; }

private:
    struct Buffer {
        DumbBufferInfo info;
        void* data = nullptr;
    };

    DrmCursor(DrmDeviceApi* device, uint32_t crtcId, const QSize& size,
              std::array<Buffer, 2> buffers);

    DrmDeviceApi* m_device = nullptr;
    uint32_t m_crtcId = 0;
    QSize m_size;
    // Two buffers, so an image change never writes into the one the display is
    // currently scanning out.
    std::array<Buffer, 2> m_buffers;
    size_t m_front = 0;
    QPoint m_hotspot;
    QPoint m_position;
    // Until the first move the plane stays where the previous DRM master left it.
    bool m_positioned = false;
    bool m_visible = false;
    // Set once the driver has rejected drmModeSetCursor; retrying every frame
    // would be pointless.
    bool m_failed = false;
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_DRM_DRM_CURSOR_H_
