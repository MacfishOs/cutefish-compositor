// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/ozone/platform/drm/common/drm_wrapper.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: wraps the compositor-owned DRM descriptor behind its
// small injectable ioctl interface.

#ifndef CUTEFISH_BACKEND_DRM_DRM_DEVICE_H_
#define CUTEFISH_BACKEND_DRM_DRM_DEVICE_H_

#include "backend/drm/drm_device_api.h"

namespace cutefish::backend {

// DrmDeviceApi over a real DRM file descriptor. The fd stays owned by
// DrmBackend, which opens it through the session.
class DrmDevice final : public DrmDeviceApi
{
public:
    explicit DrmDevice(int fd);
    ~DrmDevice() override;

    int fd() const override { return m_fd; }

    bool addFramebuffer(const FramebufferParams &params, uint32_t *framebufferId) override;
    bool removeFramebuffer(uint32_t framebufferId) override;
    bool exportCrtcFramebuffer(uint32_t crtcId, render::DmaBufAttributes *attributes) override;

    bool primeFdToHandle(int fd, uint32_t *handle) override;
    bool closeHandle(uint32_t handle) override;

    bool createPropertyBlob(const void *data, size_t size, uint32_t *id) override;
    void destroyPropertyBlob(uint32_t id) override;

    bool atomicCommit(const AtomicRequest &request, uint32_t flags, void *userData) override;

    bool setCursor(uint32_t crtcId, uint32_t handle, const QSize &size,
                   const QPoint &hotspot) override;
    bool moveCursor(uint32_t crtcId, const QPoint &position) override;

    QSize maxCursorSize() const override { return m_maxCursorSize; }

    bool createDumbBuffer(const QSize &size, DumbBufferInfo *info) override;
    bool destroyDumbBuffer(uint32_t handle) override;
    void *mapDumbBuffer(uint32_t handle, uint64_t size) override;
    void unmapDumbBuffer(void *data, uint64_t size) override;

    void invalidate() { m_fd = -1; }

private:
    int m_fd = -1;
    QSize m_maxCursorSize;
    // drmModeSetCursor2 carries the hotspot; drivers without it need the
    // caller to fold the hotspot into the cursor position instead.
    bool m_hasCursor2 = true;
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_DRM_DRM_DEVICE_H_
