// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/buffer.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "compositor/dmabuf_buffer.h"

#include "core/logging.h"

namespace cutefish::compositor {

std::unique_ptr<DmaBufBuffer> DmaBufBuffer::create(render::DmaBufDevice* device,
                                                   render::DmaBufAttributes attributes)
{
    if (!device)
        return nullptr;

    // Import before the wl_buffer exists, so an unimportable buffer is reported
    // as params.failed rather than turning up as a blank window later.
    std::shared_ptr<render::Texture> texture = device->importDmaBuf(attributes);
    if (!texture) {
        qCWarning(cfCompositor,
                  "Failed to import a %dx%d DMA-BUF, format 0x%08x modifier 0x%016llx",
                  attributes.size.width(), attributes.size.height(), attributes.format,
                  static_cast<unsigned long long>(attributes.modifier));
        return nullptr;
    }

    const render::PixelFormat format = texture->format();
    return std::unique_ptr<DmaBufBuffer>(
        new DmaBufBuffer(std::move(attributes), std::move(texture), format));
}

DmaBufBuffer::DmaBufBuffer(render::DmaBufAttributes attributes,
                           std::shared_ptr<render::Texture> texture, render::PixelFormat format)
    : m_attributes(std::move(attributes))
    , m_texture(std::move(texture))
    , m_format(format)
{
}

DmaBufBuffer::~DmaBufBuffer() = default;

int DmaBufBuffer::stride() const
{
    return m_attributes.planes.empty() ? 0 : static_cast<int>(m_attributes.planes[0].stride);
}

} // namespace cutefish::compositor
