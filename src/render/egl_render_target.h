// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "render/damage.h"
#include "render/renderer.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
struct gbm_surface;

namespace cutefish::render {

class EglSwapState
{
public:
    bool damageEnabled() const { return !m_damageFailed; }

    void damageFailed() { m_damageFailed = true; }

private:
    bool m_damageFailed = false;
};

// An EGL window surface on a gbm_surface.
class EglRenderTarget : public RenderTarget
{
public:
    EglRenderTarget(EGLDisplay display, EGLSurface surface, gbm_surface *gbmSurface,
                    const QSize &size);
    ~EglRenderTarget() override;

    QSize size() const override { return m_size; }

    EGLSurface eglSurface() const { return m_surface; }

    gbm_surface *gbmSurface() const { return m_gbmSurface; }

    static std::unique_ptr<EglRenderTarget> create(EGLDisplay display, EGLConfig config,
                                                   gbm_surface *surface, const QSize &size);
    static std::unique_ptr<EglRenderTarget> createPbuffer(EGLDisplay display, EGLConfig config,
                                                          const QSize &size);

    void beginFrame() { m_damagePrepared = false; }

    bool damagePrepared() const { return m_damagePrepared; }

    QRect prepareDamage(const QRegion &damage, const std::vector<QRect> &samples,
                        const OutputRenderContext &context);
    bool swap(EglSwapState &state);
    void invalidate();

private:
    QRect m_frameDamage;
    bool m_damagePrepared = false;
    BufferDamageHistory m_damageHistory;
    OutputRenderContext m_damageContext;
    bool m_damageValid = false;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLSurface m_surface = EGL_NO_SURFACE;
    gbm_surface *m_gbmSurface = nullptr;
    QSize m_size;
    // Resolved once: the extension string does not change for a display.
    bool m_bufferAge = false;
    PFNEGLSETDAMAGEREGIONKHRPROC m_setDamageRegion = nullptr;
    PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC m_swapBuffersWithDamage = nullptr;
};

} // namespace cutefish::render
