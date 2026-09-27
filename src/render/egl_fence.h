// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "render/egl_context.h"
#include "render/sync_fence.h"

namespace cutefish::render {
class EglFence
{
public:
    explicit EglFence(EglContext &context);
    void wait(const SyncFence &fence, bool inFrame, int timeoutMs);
    SyncFence release(bool inFrame);

private:
    EglContext &m_context;
    void *m_createSync = nullptr;
    void *m_destroySync = nullptr;
    void *m_dupNativeFence = nullptr;
    void *m_waitSyncServer = nullptr;
};
} // namespace cutefish::render
