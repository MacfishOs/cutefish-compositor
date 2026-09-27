// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/gpu/hardware_display_controller.cc
//           (SetCursor, MoveCursor, NextCursorBuffer, DrawCursor)
//           chromium/src/ui/ozone/platform/drm/gpu/drm_dumb_buffer.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#include "backend/drm/drm_cursor.h"

#include <cstring>

#include "core/logging.h"

namespace cutefish::backend {

DrmCursor::DrmCursor(DrmDeviceApi* device, uint32_t crtcId, const QSize& size,
                     std::array<Buffer, 2> buffers)
    : m_device(device)
    , m_crtcId(crtcId)
    , m_size(size)
    , m_buffers(buffers)
{
}

DrmCursor::~DrmCursor()
{
    hide();
    for (Buffer& buffer : m_buffers) {
        m_device->unmapDumbBuffer(buffer.data, buffer.info.size);
        if (buffer.info.handle)
            m_device->destroyDumbBuffer(buffer.info.handle);
    }
}

std::unique_ptr<DrmCursor> DrmCursor::create(DrmDeviceApi* device, uint32_t crtcId)
{
    if (!device || !crtcId)
        return nullptr;

    const QSize size = device->maxCursorSize();
    if (size.isEmpty())
        return nullptr;

    std::array<Buffer, 2> buffers = {};
    for (Buffer& buffer : buffers) {
        if (!device->createDumbBuffer(size, &buffer.info))
            break;
        buffer.data = device->mapDumbBuffer(buffer.info.handle, buffer.info.size);
        if (!buffer.data)
            break;
        std::memset(buffer.data, 0, buffer.info.size);
    }

    const bool allocated = buffers[0].data && buffers[1].data;
    if (!allocated) {
        for (Buffer& buffer : buffers) {
            device->unmapDumbBuffer(buffer.data, buffer.info.size);
            if (buffer.info.handle)
                device->destroyDumbBuffer(buffer.info.handle);
        }
        qCInfo(cfDrm) << "No hardware cursor buffers; the cursor stays composited";
        return nullptr;
    }

    return std::unique_ptr<DrmCursor>(new DrmCursor(device, crtcId, size, buffers));
}

bool DrmCursor::setImage(const uint8_t* pixels, const QSize& size, int stride,
                         const QPoint& hotspot)
{
    if (m_failed || !pixels || size.isEmpty())
        return false;
    if (size.width() > m_size.width() || size.height() > m_size.height())
        return false;

    // Never write into the buffer the display is scanning out.
    const size_t back = m_visible ? (m_front ^ 1) : m_front;
    Buffer& buffer = m_buffers[back];

    std::memset(buffer.data, 0, buffer.info.size);
    auto* destination = static_cast<uint8_t*>(buffer.data);
    const int rowBytes = size.width() * 4;
    for (int y = 0; y < size.height(); ++y) {
        std::memcpy(destination + static_cast<size_t>(y) * buffer.info.stride,
                    pixels + static_cast<size_t>(y) * stride, static_cast<size_t>(rowBytes));
    }

    m_hotspot = hotspot;
    if (!m_device->setCursor(m_crtcId, buffer.info.handle, m_size, hotspot)) {
        qCInfo(cfDrm) << "The driver rejected the hardware cursor; compositing it instead";
        m_failed = true;
        m_visible = false;
        return false;
    }

    m_front = back;
    m_visible = true;
    // A new image on an already-positioned cursor still has to be placed: the
    // hotspot may have moved within the image.
    if (m_positioned)
        m_device->moveCursor(m_crtcId, m_position - m_hotspot);
    return true;
}

void DrmCursor::hide()
{
    if (!m_visible)
        return;
    m_visible = false;
    m_device->setCursor(m_crtcId, 0, QSize(), QPoint());
}

void DrmCursor::move(const QPoint& position)
{
    m_position = position;
    m_positioned = true;
    if (m_visible)
        m_device->moveCursor(m_crtcId, position - m_hotspot);
}

} // namespace cutefish::backend
