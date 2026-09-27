// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/wayland/wayland_dmabuf_feedback_manager.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "wayland/protocols/dmabuf_feedback_manager.h"

#include <drm_fourcc.h>
#include <linux-dmabuf-v1-server-protocol.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <set>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "render/dmabuf.h"

namespace cutefish::wayland {

namespace {

enum TrancheFlags : uint32_t { kNone = 0, kScanout = 1 };

// The wire layout of one format table entry.
struct DmaBufFeedbackFormat {
    uint32_t format;
    uint32_t padding;
    uint64_t modifier;
};

void feedback_destroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

const struct zwp_linux_dmabuf_feedback_v1_interface feedback_implementation = {
    feedback_destroy,
};

// Intel CCS modifiers leak memory on gbm to gl buffer import. Block these
// modifiers for now. See crbug.com/1445252, crbug.com/1458575.
// Also blocking |DRM_FORMAT_YVU420| for a specific modifier based on a test
// failing in minigbm. See b/289714323.
const std::set<std::pair<uint64_t, uint64_t>>& modifierBlockList()
{
    static const std::set<std::pair<uint64_t, uint64_t>> list = {
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_Y_TILED_CCS },
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_Yf_TILED_CCS },
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS },
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS },
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC },
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_4_TILED_DG2_RC_CCS },
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_4_TILED_DG2_MC_CCS },
        { DRM_FORMAT_INVALID, I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC },
        { DRM_FORMAT_YVU420, DRM_FORMAT_MOD_QCOM_COMPRESSED },
    };
    return list;
}

// A sealed memfd holding `table`. Clients map it read-only, so it must not be
// growable or writable once handed out.
ScopedFd createFormatTable(const std::vector<DmaBufFeedbackFormat>& table)
{
    const size_t size = table.size() * sizeof(DmaBufFeedbackFormat);
    if (size == 0)
        return ScopedFd();

    ScopedFd fd(memfd_create("cutefish-dmabuf-format-table",
                             MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd.isValid()) {
        qCWarning(cfWayland) << "memfd_create failed for the dmabuf format table";
        return ScopedFd();
    }

    if (ftruncate(fd.get(), static_cast<off_t>(size)) != 0) {
        qCWarning(cfWayland) << "ftruncate failed for the dmabuf format table";
        return ScopedFd();
    }

    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (data == MAP_FAILED) {
        qCWarning(cfWayland) << "mmap failed for the dmabuf format table";
        return ScopedFd();
    }
    std::memcpy(data, table.data(), size);
    munmap(data, size);

    if (fcntl(fd.get(), F_ADD_SEALS,
              F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)
        != 0) {
        qCWarning(cfWayland) << "Failed to seal the dmabuf format table";
        return ScopedFd();
    }

    return fd;
}

} // namespace

// A tranche links to a subset of formats and modifiers which are supported on a
// specific device for a specific task. The most common use cases are the
// "default" tranche (targeting the main device, advertising all formats and
// modifiers supported by the compositor, for non-scanout cases) and "scanout"
// tranches, which Cutefish does not send.
class DmaBufFeedbackTranche
{
public:
    DmaBufFeedbackTranche(dev_t targetDeviceId, TrancheFlags flags,
                          IndexedDrmFormatsAndModifiers formatsAndModifiers)
        : m_targetDeviceId(targetDeviceId)
        , m_flags(flags)
        , m_formatsAndModifiers(std::move(formatsAndModifiers))
    {
    }

    DmaBufFeedbackTranche(const DmaBufFeedbackTranche&) = delete;
    DmaBufFeedbackTranche& operator=(const DmaBufFeedbackTranche&) = delete;

    dev_t targetDeviceId() const { return m_targetDeviceId; }

    TrancheFlags flags() const { return m_flags; }

    const IndexedDrmFormatsAndModifiers& formatsAndModifiers() const
    {
        return m_formatsAndModifiers;
    }

private:
    const dev_t m_targetDeviceId;
    const TrancheFlags m_flags;
    const IndexedDrmFormatsAndModifiers m_formatsAndModifiers;
};

// A feedback is the set of information sent to a client: a main device (the
// device the compositor renders on), a format table shared through an fd, and
// one or more tranches, of which the default one must always be present.
class DmaBufFeedback
{
public:
    DmaBufFeedback(dev_t deviceId, std::unique_ptr<DmaBufFeedbackTranche> defaultTranche)
        : m_mainDeviceId(deviceId)
        , m_defaultTranche(std::move(defaultTranche))
    {
    }

    DmaBufFeedback(const DmaBufFeedback&) = delete;
    DmaBufFeedback& operator=(const DmaBufFeedback&) = delete;

    dev_t mainDeviceId() const { return m_mainDeviceId; }

    const DmaBufFeedbackTranche* defaultTranche() const { return m_defaultTranche.get(); }

private:
    const dev_t m_mainDeviceId;
    const std::unique_ptr<DmaBufFeedbackTranche> m_defaultTranche;
};

DmaBufFeedbackManager::DmaBufFeedbackManager(compositor::Compositor* compositor,
                                             render::DmaBufDevice* device)
    : m_compositor(compositor)
{
    std::vector<DmaBufFeedbackFormat> table;
    size_t formatTableIndex = 0;

    for (const auto& [drmFormat, modifiers] : device->dmaBufFormats()) {
        std::map<size_t, uint64_t> modifierEntries;
        // The implicit modifier is always importable and always comes first.
        modifierEntries.emplace(formatTableIndex++, DRM_FORMAT_MOD_INVALID);
        table.push_back({ drmFormat, 0, DRM_FORMAT_MOD_INVALID });

        for (uint64_t modifier : modifiers) {
            if (modifier == DRM_FORMAT_MOD_INVALID)
                continue;
            // Check for generic blocking first then format specific blocking.
            if (modifierBlockList().contains({ DRM_FORMAT_INVALID, modifier })
                || modifierBlockList().contains({ drmFormat, modifier })) {
                continue;
            }
            modifierEntries.emplace(formatTableIndex++, modifier);
            table.push_back({ drmFormat, 0, modifier });
        }

        m_formatsAndModifiers.emplace(drmFormat, std::move(modifierEntries));
    }

    const dev_t mainDevice = device->dmaBufMainDevice();
    if (!mainDevice) {
        m_version = ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION;
        return;
    }

    m_formatTableFd = createFormatTable(table);
    if (!m_formatTableFd.isValid()) {
        m_version = ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION;
        return;
    }
    m_formatTableSize = table.size() * sizeof(DmaBufFeedbackFormat);

    auto tranche = std::make_unique<DmaBufFeedbackTranche>(mainDevice, TrancheFlags::kNone,
                                                           m_formatsAndModifiers);
    m_defaultFeedback = std::make_unique<DmaBufFeedback>(mainDevice, std::move(tranche));

    m_version = ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION;
}

DmaBufFeedbackManager::~DmaBufFeedbackManager() = default;

bool DmaBufFeedbackManager::isFormatSupported(uint32_t format) const
{
    return m_formatsAndModifiers.contains(format);
}

void DmaBufFeedbackManager::sendFormatsAndModifiers(wl_resource* resource) const
{
    for (const auto& [format, modifierEntries] : m_formatsAndModifiers) {
        zwp_linux_dmabuf_v1_send_format(resource, format);
        if (wl_resource_get_version(resource) >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
            for (const auto& [tableIndex, modifier] : modifierEntries) {
                zwp_linux_dmabuf_v1_send_modifier(resource, format, modifier >> 32,
                                                  modifier & 0xffffffff);
            }
        }
    }
}

void DmaBufFeedbackManager::getDefaultFeedback(wl_client* client, wl_resource* dmaBufResource,
                                               uint32_t feedbackId)
{
    wl_resource* feedbackResource =
        wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                           wl_resource_get_version(dmaBufResource), feedbackId);
    if (!feedbackResource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(feedbackResource, &feedback_implementation, nullptr, nullptr);

    sendFeedback(m_defaultFeedback.get(), feedbackResource);
}

void DmaBufFeedbackManager::getSurfaceFeedback(wl_client* client, wl_resource* dmaBufResource,
                                               uint32_t feedbackId, wl_resource* surfaceResource)
{
    // Per-surface feedback carries the default feedback until scanout tranches
    // exist; the request must still produce a working object.
    (void)surfaceResource;
    getDefaultFeedback(client, dmaBufResource, feedbackId);
}

void DmaBufFeedbackManager::sendFeedback(const DmaBufFeedback* feedback,
                                         wl_resource* resource) const
{
    if (!feedback)
        return;

    wl_array mainDeviceBuf;
    wl_array_init(&mainDeviceBuf);
    auto* deviceIdPtr = static_cast<dev_t*>(wl_array_add(&mainDeviceBuf, sizeof(dev_t)));
    *deviceIdPtr = feedback->mainDeviceId();
    zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &mainDeviceBuf);
    wl_array_release(&mainDeviceBuf);

    zwp_linux_dmabuf_feedback_v1_send_format_table(resource, m_formatTableFd.get(),
                                                   m_formatTableSize);

    sendTranche(feedback->defaultTranche(), resource);

    zwp_linux_dmabuf_feedback_v1_send_done(resource);
}

void DmaBufFeedbackManager::sendTranche(const DmaBufFeedbackTranche* tranche,
                                        wl_resource* resource) const
{
    wl_array targetDeviceBuf;
    wl_array_init(&targetDeviceBuf);
    auto* deviceIdPtr = static_cast<dev_t*>(wl_array_add(&targetDeviceBuf, sizeof(dev_t)));
    *deviceIdPtr = tranche->targetDeviceId();
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &targetDeviceBuf);
    wl_array_release(&targetDeviceBuf);

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, tranche->flags());

    wl_array formatsArray;
    wl_array_init(&formatsArray);
    for (const auto& [format, modifierEntries] : tranche->formatsAndModifiers()) {
        for (const auto& [tableIndex, modifier] : modifierEntries) {
            auto* formatIndexPtr =
                static_cast<uint16_t*>(wl_array_add(&formatsArray, sizeof(uint16_t)));
            *formatIndexPtr = static_cast<uint16_t>(tableIndex);
        }
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &formatsArray);
    wl_array_release(&formatsArray);

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
}

} // namespace cutefish::wayland
