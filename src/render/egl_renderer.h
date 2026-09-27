// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_RENDER_EGL_RENDERER_H_
#define CUTEFISH_RENDER_EGL_RENDERER_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "render/backdrop_filter.h"
#include "render/dmabuf.h"
#include "render/drop_shadow.h"
#include "render/egl_context.h"
#include "render/egl_dmabuf_importer.h"
#include "render/egl_fence.h"
#include "render/egl_render_target.h"
#include "render/gl_program.h"
#include "render/gl_render_pass.h"
#include "render/renderer.h"

struct gbm_device;
struct gbm_surface;

namespace cutefish::render {

// OpenGL ES 2 renderer on the EGL GBM platform. Owns the EGL display, config
// and context; DRM/KMS lives entirely outside it.
class EglRenderer : public Renderer, public DmaBufDevice
{
public:
    ~EglRenderer() override;

    // `format` is the DRM fourcc the GBM surfaces will use; the EGL config is
    // matched against it through EGL_NATIVE_VISUAL_ID.
    static std::unique_ptr<EglRenderer> create(gbm_device *device, uint32_t format);

    std::unique_ptr<EglRenderTarget> createTarget(gbm_surface *surface, const QSize &size);

    // Called before an output destroys its target. Retire renderer-owned
    // textures while this target/context still exists and forget borrowed pointers.
    void releaseTarget(EglRenderTarget *target);

    void setOutputContext(OutputRenderContext context) override;
    bool beginFrame(RenderTarget *target) override;
    bool endFrame() override;
    void cancelFrame() override;
    void prepareDamage(const QRegion &damage, const std::vector<QRect> &samples) override;
    bool repaints(const QRect &logical) const override;

    void waitFence(const SyncFence &fence) override;
    SyncFence createReleaseFence() override;

    void clear(const Color &color) override;
    void fillRect(const QRect &rect, const Color &color) override;
    void drawRoundedOutline(const RoundedShape &shape, const Color &color, qreal widthPx) override;
    void fillRectF(const QRectF &rect, const Color &color) override;
    void drawTextureF(Texture *texture, const QRectF &target, float alpha) override;
    void drawTextureTransformed(Texture *texture, const QRectF &target, const QTransform &transform,
                                float alpha) override;
    void drawBackgroundBlur(const QRect &bounds, int radius, const QRect &clip,
                            const GlassParams &glass = {}, const QPainterPath &shape = {}) override;
    void drawTexture(Texture *texture, const QRect &target, float alpha) override;
    void drawTextureMapped(Texture *texture, const QRect &target, float alpha,
                           const QTransform &mapping) override;

    std::unique_ptr<Texture> createTexture(const QSize &size, const uint8_t *pixels, int stride,
                                           PixelFormat format) override;
    bool updateTexture(Texture *texture, const uint8_t *pixels, int stride,
                       PixelFormat format) override;
    bool updateTexture(Texture *texture, const uint8_t *pixels, int stride, PixelFormat format,
                       const QRegion &damage) override;
    using Renderer::createTexture;

    QImage captureFrame(const std::function<void()> &paint) override;
    QImage captureImage(const OutputRenderContext &context, const QSize &size,
                        const std::function<void()> &paint) override;
    std::shared_ptr<PendingImage> captureFrameLater(const std::function<void()> &paint) override;
    std::shared_ptr<PendingImage> captureImageLater(const OutputRenderContext &context,
                                                    const QSize &size,
                                                    const std::function<void()> &paint) override;
    bool captureToDmaBuf(const OutputRenderContext &context, const QSize &size,
                         const DmaBufAttributes &attributes,
                         const std::function<void()> &paint) override;
    const DmaBufFormats &captureDmaBufFormats() const override;
    dev_t captureDmaBufDevice() const override;
    QImage renderToImage(const QSize &size, const std::function<void()> &paint) override;
    bool renderToTexture(std::unique_ptr<Texture> &texture, const QRect &bounds,
                         const std::function<void()> &paint) override;
    bool renderToTexture(std::unique_ptr<Texture> &texture, const QRect &bounds,
                         const QRect &repaint, const std::function<void()> &paint) override;
    void enableTrilinearFiltering(Texture *texture) override;
    void limitMipmapLevel(Texture *texture, int level) override;
    std::unique_ptr<Texture> renderShadow(const QSize &size, const std::vector<ShadowLobe> &lobes,
                                          const std::function<void()> &paintCaster) override;

    // DmaBufDevice.
    const DmaBufFormats &dmaBufFormats() const override { return m_importer.formats(); }

    dev_t dmaBufMainDevice() const override { return m_importer.mainDevice(); }

    std::shared_ptr<Texture> importDmaBuf(const DmaBufAttributes &attributes) override;

    QSize viewportSize() const override { return m_pass.viewport(); }

    EGLDisplay eglDisplay() const { return m_context->display(); }

    EGLConfig eglConfig() const { return m_context->config(); }

    QByteArray eglVendor() const { return m_context->vendor(); }

    QByteArray eglVersion() const { return m_context->version(); }

    QByteArray glesRenderer() const { return m_context->renderer(); }

    QByteArray glesVersion() const { return m_context->glVersion(); }

private:
    friend class EglRendererTestAccess;
    explicit EglRenderer(std::shared_ptr<EglContext> context, gbm_device *device = nullptr);
    bool initPrograms(const char *fragmentOverride = nullptr,
                      const char *externalOverride = nullptr,
                      const char *backdropOverride = nullptr);
    bool makeCurrent(EglRenderTarget *target);
    void applyRepaint(const QRect &repaint);
    void drawTextureMappedF(Texture *texture, const QRectF &target, float alpha,
                            const QTransform &mapping, const QTransform &geometry = QTransform());
    void applyRoundedClip(const GlProgram &program);
    void fillColorRects(std::initializer_list<QRectF> rects, const Color &color);
    void quadFor(const QRectF &rect, float *vertices) const;
    // Corners in triangle-strip order: top left, top right, bottom left, bottom right.
    void quadFor(const std::array<QPointF, 4> &corners, float *vertices) const;

    std::shared_ptr<EglContext> m_context;
    EglFence m_fences;
    EglDmaBufImporter m_importer;
    gbm_device *m_device = nullptr;
    // Probed on first use: which DMA-BUFs this GPU can render captures into.
    mutable std::optional<DmaBufFormats> m_captureFormats;
    mutable dev_t m_captureDevice = 0;
    GlTextureUploader m_upload;
    GlRenderPass m_pass;
    std::unique_ptr<BackdropFilter> m_backdrop;
    std::unique_ptr<DropShadow> m_dropShadow;
    GlProgram m_colorProgram;
    GlProgram m_textureProgram;
    GlProgram m_externalProgram;
    RoundedShape m_outlineShape;
    qreal m_outlineWidth = 0;
    EglSwapState m_swapState;
    // The scissor of a partial repaint, in target pixels.
    std::optional<QRect> m_repaint;
    EglRenderTarget *m_current = nullptr;
    EglRenderTarget *m_lastTarget = nullptr;
};

} // namespace cutefish::render

#endif // CUTEFISH_RENDER_EGL_RENDERER_H_
