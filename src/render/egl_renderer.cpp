// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/egl_renderer.h"

#include <GLES3/gl3.h>
// Extension declarations require the core GL types first.
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <cmath>
#include <drm_fourcc.h>
#include <gbm.h>
#include <sys/stat.h>
#include <xf86drm.h>

#include "core/logging.h"
#include "render_shaders.h"

namespace cutefish::render {

EglRenderer::EglRenderer(std::shared_ptr<EglContext> context, gbm_device *device)
    : m_context(std::move(context))
    , m_fences(*m_context)
    , m_importer(m_context, device)
    , m_upload(m_context)
    , m_pass(m_context)
    , m_device(device)
{
}

EglRenderer::~EglRenderer()
{
    if (!m_colorProgram && !m_textureProgram && !m_externalProgram && !m_backdrop
        && !m_dropShadow && !textureCacheSize())
        return;
    ScopedEglCurrent current(*m_context);
    if (!current.valid())
        qFatal("Cannot make EGL context current for renderer retirement");
    m_backdrop.reset();
    m_dropShadow.reset();
    clearTextureCache();
    m_colorProgram.reset();
    m_textureProgram.reset();
    m_externalProgram.reset();
}

std::unique_ptr<EglRenderer> EglRenderer::create(gbm_device *device, uint32_t format)
{
    auto context = EglContext::create(device, format);
    if (!context)
        return nullptr;
    return std::unique_ptr<EglRenderer>(new EglRenderer(std::move(context), device));
}

std::unique_ptr<EglRenderTarget> EglRenderer::createTarget(gbm_surface *surface, const QSize &size)
{
    auto target = EglRenderTarget::create(m_context->display(), m_context->config(), surface, size);
    if (!target || !m_context->makeCurrent(target->eglSurface()))
        return nullptr;
    if (!m_colorProgram && !initPrograms()) {
        m_context->makeCurrent(m_lastTarget ? m_lastTarget->eglSurface() : EGL_NO_SURFACE);
        return nullptr;
    }
    m_lastTarget = target.get();
    eglSwapInterval(m_context->display(), 1);
    return target;
}

bool EglRenderer::initPrograms(const char *fragmentOverride, const char *externalOverride,
                               const char *backdropOverride)
{
    auto color =
            GlProgram::create(shaders::color_vertex_shader,
                              fragmentOverride ? QByteArray(fragmentOverride)
                                               : clippedFragment(shaders::color_fragment_shader));
    auto texture = GlProgram::create(shaders::texture_vertex_shader,
                                     clippedFragment(shaders::texture_fragment_shader));
    if (!color || !texture)
        return false;
    auto external =
            GlProgram::create(shaders::texture_vertex_shader,
                              externalOverride ? QByteArray(externalOverride)
                                               : clippedFragment(shaders::external_fragment_shader,
                                                                 shaders::external_extension));
    if (!external)
        qCWarning(cfRender)
                << "GL_OES_EGL_image_external is unavailable; DMA-BUF buffers cannot be composited";
    // Blur and glass are effects: a driver that cannot build them still gets a
    // desktop, drawn without them.
    auto backdrop = std::make_unique<BackdropFilter>(m_context);
    if (backdrop->initialize(backdropOverride))
        m_backdrop = std::move(backdrop);
    else
        qCWarning(cfRender) << "Blur and glass shaders are unavailable; drawing without them";
    auto dropShadow = std::make_unique<DropShadow>();
    if (dropShadow->initialize())
        m_dropShadow = std::move(dropShadow);
    else
        qCWarning(cfRender) << "The shadow shader is unavailable; drawing without shadows";
    m_colorProgram = std::move(color);
    m_textureProgram = std::move(texture);
    m_externalProgram = std::move(external);
    return true;
}

bool EglRenderer::makeCurrent(EglRenderTarget *target)
{
    if (!target || !m_context->makeCurrent(target->eglSurface()))
        return false;
    m_lastTarget = target;
    return true;
}

void EglRenderer::releaseTarget(EglRenderTarget *target)
{
    if (!target || target != m_lastTarget)
        return;
    if (!makeCurrent(target))
        qFatal("Cannot make EGL context current for output texture retirement");
    if (m_backdrop)
        m_backdrop->resetTextures();
    clearTextureCache();
    m_current = nullptr;
    m_lastTarget = nullptr;
}

void EglRenderer::waitFence(const SyncFence &fence)
{
    m_fences.wait(fence, m_current != nullptr, kFenceWaitMs);
}

SyncFence EglRenderer::createReleaseFence()
{
    return m_fences.release(m_current != nullptr);
}

std::unique_ptr<Texture> EglRenderer::createTexture(const QSize &size, const uint8_t *pixels,
                                                    int stride, PixelFormat format)
{
    if (!m_current && !makeCurrent(m_lastTarget))
        return nullptr;
    return m_upload.create(size, pixels, stride, format);
}

bool EglRenderer::updateTexture(Texture *texture, const uint8_t *pixels, int stride,
                                PixelFormat format)
{
    if (!m_current && !makeCurrent(m_lastTarget))
        return false;
    return m_upload.update(texture, pixels, stride, format);
}

bool EglRenderer::updateTexture(Texture *texture, const uint8_t *pixels, int stride,
                                PixelFormat format, const QRegion &damage)
{
    if (!m_current && !makeCurrent(m_lastTarget))
        return false;
    return m_upload.update(texture, pixels, stride, format, damage);
}

std::shared_ptr<Texture> EglRenderer::importDmaBuf(const DmaBufAttributes &attributes)
{
    if ((!m_current && !makeCurrent(m_lastTarget)) || !m_externalProgram)
        return nullptr;
    return m_importer.import(attributes);
}

QImage EglRenderer::captureFrame(const std::function<void()> &paint)
{
    if (!m_current || m_pass.viewport().isEmpty())
        return {};
    return m_pass.capture(*this, m_current->size(), paint);
}

QImage EglRenderer::captureImage(const OutputRenderContext &context, const QSize &size,
                                 const std::function<void()> &paint)
{
    if (!m_current)
        return {};
    ScopedValue clips(m_roundedClips);
    m_roundedClips.clear();
    return m_pass.captureImage(*this, context, size, paint);
}

std::shared_ptr<PendingImage> EglRenderer::captureFrameLater(const std::function<void()> &paint)
{
    if (!m_current || m_pass.viewport().isEmpty())
        return nullptr;
    return m_pass.readBack(*this, outputContext(), m_current->size(), paint);
}

std::shared_ptr<PendingImage> EglRenderer::captureImageLater(const OutputRenderContext &context,
                                                             const QSize &size,
                                                             const std::function<void()> &paint)
{
    if (!m_current)
        return nullptr;
    ScopedValue clips(m_roundedClips);
    m_roundedClips.clear();
    return m_pass.readBack(*this, context, size, paint);
}

bool EglRenderer::captureToDmaBuf(const OutputRenderContext &context, const QSize &size,
                                  const DmaBufAttributes &attributes,
                                  const std::function<void()> &paint)
{
    if (!m_current || attributes.size != size)
        return false;
    const auto &formats = captureDmaBufFormats();
    const auto format = formats.find(attributes.format);
    if (format == formats.end()
        || std::find(format->second.begin(), format->second.end(), attributes.modifier)
                == format->second.end())
        return false;
    EGLImageKHR image = m_importer.createImage(attributes);
    if (image == EGL_NO_IMAGE_KHR)
        return false;
    ScopedValue clips(m_roundedClips);
    m_roundedClips.clear();
    const bool ok = m_pass.captureToImage(*this, context, size, image, paint);
    auto destroyImage =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    if (destroyImage)
        destroyImage(m_context->display(), image);
    return ok;
}

// Whether this GPU renders into a buffer allocated the way a capture client
// would allocate it: one probe per format, with a linear layout, so a client
// never learns of a format the frame path would then refuse.
const DmaBufFormats &EglRenderer::captureDmaBufFormats() const
{
    if (m_captureFormats)
        return *m_captureFormats;
    m_captureFormats.emplace();
    if (!m_device)
        return *m_captureFormats;
    ScopedEglCurrent current(*m_context);
    auto imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!current.valid() || !imageTargetTexture)
        return *m_captureFormats;

    const int fd = gbm_device_get_fd(m_device);
    struct stat deviceStat = {};
    if (char *node = drmGetRenderDeviceNameFromFd(fd)) {
        if (stat(node, &deviceStat) == 0)
            m_captureDevice = deviceStat.st_rdev;
        free(node);
    }
    if (!m_captureDevice)
        m_captureDevice = m_importer.mainDevice();

    const auto renderable = [&](gbm_bo *bo, uint32_t format, uint64_t modifier) {
        DmaBufAttributes attributes;
        attributes.size = QSize(16, 16);
        attributes.format = format;
        attributes.modifier = modifier;
        DmaBufPlane plane;
        plane.fd = ScopedFd(gbm_bo_get_fd(bo));
        plane.stride = gbm_bo_get_stride(bo);
        attributes.planes.push_back(std::move(plane));
        if (attributes.planes.front().fd.get() < 0)
            return false;
        EGLImageKHR image = m_importer.createImage(attributes);
        if (image == EGL_NO_IMAGE_KHR)
            return false;
        GLuint name = 0, framebuffer = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        imageTargetTexture(GL_TEXTURE_2D, image);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, name, 0);
        const bool ok = glGetError() == GL_NO_ERROR
                && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &name);
        auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR"));
        if (destroyImage)
            destroyImage(m_context->display(), image);
        return ok;
    };

    for (const uint32_t format : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888}) {
        std::vector<uint64_t> modifiers;
        // Clients that name modifiers (PipeWire consumers such as Sunshine)
        // never offer INVALID, so an explicit LINEAR is needed to meet them.
        const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
        if (gbm_bo *bo = gbm_bo_create_with_modifiers2(m_device, 16, 16, format, &linear, 1,
                                                       GBM_BO_USE_RENDERING)) {
            if (gbm_bo_get_modifier(bo) == DRM_FORMAT_MOD_LINEAR
                && renderable(bo, format, DRM_FORMAT_MOD_LINEAR))
                modifiers.push_back(DRM_FORMAT_MOD_LINEAR);
            gbm_bo_destroy(bo);
        }
        // Some GBM backends (virgl among them) only allocate through the
        // flag, and report the result as INVALID.
        if (gbm_bo *bo = gbm_bo_create(m_device, 16, 16, format,
                                       GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)) {
            const uint64_t modifier = gbm_bo_get_modifier(bo);
            if (std::find(modifiers.begin(), modifiers.end(), modifier) == modifiers.end()
                && renderable(bo, format, modifier))
                modifiers.push_back(modifier);
            gbm_bo_destroy(bo);
        }
        if (!modifiers.empty())
            m_captureFormats->emplace(format, std::move(modifiers));
    }
    qCInfo(cfRender, "Captures can render into %zu DMA-BUF formats",
           m_captureFormats->size());
    return *m_captureFormats;
}

dev_t EglRenderer::captureDmaBufDevice() const
{
    captureDmaBufFormats();
    return m_captureDevice;
}

QImage EglRenderer::renderToImage(const QSize &size, const std::function<void()> &paint)
{
    if (!m_current)
        return {};
    ScopedValue clips(m_roundedClips);
    m_roundedClips.clear();
    return m_pass.renderToImage(*this, size, paint);
}

bool EglRenderer::renderToTexture(std::unique_ptr<Texture> &texture, const QRect &bounds,
                                  const std::function<void()> &paint)
{
    return renderToTexture(texture, bounds, QRect(), paint);
}

bool EglRenderer::renderToTexture(std::unique_ptr<Texture> &texture, const QRect &bounds,
                                  const QRect &repaint, const std::function<void()> &paint)
{
    if (!m_current)
        return false;
    return m_pass.render(*this, texture, bounds, paint, repaint);
}

std::unique_ptr<Texture> EglRenderer::renderShadow(const QSize &size,
                                                   const std::vector<ShadowLobe> &lobes,
                                                   const std::function<void()> &paintCaster)
{
    if (!m_current || !m_dropShadow)
        return nullptr;
    // A clip of the node being painted would cut into the caster.
    ScopedValue clips(m_roundedClips);
    m_roundedClips.clear();
    const double scale = rasterScale();
    setRasterScale(1);
    auto texture = m_dropShadow->render(*this, size, lobes, paintCaster);
    setRasterScale(scale);
    return texture;
}

void EglRenderer::enableTrilinearFiltering(Texture *texture)
{
    if (m_current)
        m_pass.generateMipmaps(texture);
}

void EglRenderer::limitMipmapLevel(Texture *texture, int level)
{
    if (m_current)
        m_pass.limitMipmapLevel(texture, level);
}

void EglRenderer::prepareDamage(const QRegion &damage, const std::vector<QRect> &samples)
{
    if (!m_current || capturing || m_current->damagePrepared())
        return;
    applyRepaint(m_current->prepareDamage(damage, samples, outputContext()));
}

bool EglRenderer::repaints(const QRect &logical) const
{
    if (!m_current || capturing || !m_repaint)
        return true;
    return m_repaint->intersects(physicalDamage(logical, outputContext(), m_current->size()));
}

bool EglRenderer::endFrame()
{
    if (!m_current)
        return false;
    const bool ok = m_current->swap(m_swapState);
    if (m_backdrop)
        m_backdrop->frameEnded();
    glDisable(GL_SCISSOR_TEST);
    m_repaint.reset();
    m_current = nullptr;
    return ok;
}

void EglRenderer::cancelFrame()
{
    glDisable(GL_SCISSOR_TEST);
    m_repaint.reset();
    if (m_current)
        m_current->invalidate();
    m_current = nullptr;
}

void EglRenderer::setOutputContext(OutputRenderContext context)
{
    Renderer::setOutputContext(std::move(context));
    if (m_current)
        beginFrame(m_current);
}

bool EglRenderer::beginFrame(RenderTarget *target)
{
    auto *eglTarget = static_cast<EglRenderTarget *>(target);
    if (!makeCurrent(eglTarget))
        return false;

    m_current = eglTarget;
    const QRect content = outputContext().nativeContentRect.isEmpty()
            ? QRect(QPoint(), eglTarget->size())
            : outputContext().nativeContentRect;
    m_current->beginFrame();
    m_pass.setGeometry(content.size(), outputContext().logicalViewport.topLeft());
    glViewport(content.x(), eglTarget->size().height() - content.y() - content.height(),
               content.width(), content.height());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    m_repaint.reset();
    return true;
}

void EglRenderer::applyRepaint(const QRect &repaint)
{
    m_repaint = repaint;
    glEnable(GL_SCISSOR_TEST);
    glScissor(repaint.x(), m_current->size().height() - repaint.y() - repaint.height(),
              repaint.width(), repaint.height());
}

void EglRenderer::clear(const Color &color)
{
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void EglRenderer::quadFor(const QRectF &rect, float *vertices) const
{
    quadFor({rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()}, vertices);
}

void EglRenderer::quadFor(const std::array<QPointF, 4> &corners, float *vertices) const
{
    const bool quarterTurn = outputContext().transform == 1 || outputContext().transform == 3;
    const float width =
            (quarterTurn ? m_pass.viewport().height() : m_pass.viewport().width()) / rasterScale();
    const float height =
            (quarterTurn ? m_pass.viewport().width() : m_pass.viewport().height()) / rasterScale();
    if (width <= 0.0f || height <= 0.0f)
        return;

    const auto map = [&](const QPointF &point) {
        QPointF normalized(point.x() / width, point.y() / height);
        switch (outputContext().transform) {
        case 1:
            normalized = {normalized.y(), 1.0 - normalized.x()};
            break;
        case 2:
            normalized = {1.0 - normalized.x(), 1.0 - normalized.y()};
            break;
        case 3:
            normalized = {1.0 - normalized.y(), normalized.x()};
            break;
        default:
            break;
        }
        return QPointF(normalized.x() * 2.0 - 1.0, 1.0 - normalized.y() * 2.0);
    };
    for (int index = 0; index < 4; ++index) {
        const QPointF point = map(corners[index]);
        vertices[index * 2] = point.x();
        vertices[index * 2 + 1] = point.y();
    }
}

void EglRenderer::applyRoundedClip(const GlProgram &program)
{
    glUniform1i(program.uniform("uClipCount"), int(m_roundedClips.size()));
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glUniform4f(program.uniform("uViewportRect"), viewport[0], viewport[1], viewport[2],
                viewport[3]);
    glUniform1i(program.uniform("uOutputTransform"), outputContext().transform);
    const auto device = [&](const RoundedShape &shape) {
        return QRectF((shape.bounds.topLeft() - m_pass.offset()) * rasterScale(),
                      shape.bounds.size() * rasterScale());
    };
    float bounds[16 * 4] = {};
    float radii[32] = {};
    const size_t count = std::min(m_roundedClips.size(), size_t(16));
    for (size_t i = 0; i < count; ++i) {
        const QRectF rect = device(m_roundedClips[i]);
        bounds[i * 4] = rect.x();
        bounds[i * 4 + 1] = rect.y();
        bounds[i * 4 + 2] = rect.width();
        bounds[i * 4 + 3] = rect.height();
        radii[i * 2] =
                m_roundedClips[i].radius * m_roundedClips[i].radiusScale.width() * rasterScale();
        radii[i * 2 + 1] =
                m_roundedClips[i].radius * m_roundedClips[i].radiusScale.height() * rasterScale();
    }
    glUniform4fv(program.uniform("uClips"), 16, bounds);
    glUniform2fv(program.uniform("uClipRadii"), 16, radii);
    const QRectF outline = device(m_outlineShape);
    glUniform4f(program.uniform("uOutlineBounds"), outline.x(), outline.y(), outline.width(),
                outline.height());
    glUniform2f(program.uniform("uOutlineRadius"),
                m_outlineShape.radius * m_outlineShape.radiusScale.width() * rasterScale(),
                m_outlineShape.radius * m_outlineShape.radiusScale.height() * rasterScale());
    glUniform1f(program.uniform("uOutlineWidth"), m_outlineWidth);
    glUniform1f(program.uniform("uContinuousCorners"), continuousCorners() ? 1.0f : 0.0f);
}

void EglRenderer::drawRoundedOutline(const RoundedShape &shape, const Color &color, qreal widthPx)
{
    if (widthPx <= 0)
        return;
    m_outlineShape = shape;
    m_outlineWidth = widthPx;
    const qreal fringe = 0.5 / rasterScale();
    const QRectF bounds = shape.bounds.adjusted(-fringe, -fringe, fringe, fringe);
    const qreal bandX =
            std::min(bounds.width() / 2,
                     std::max(shape.radius * shape.radiusScale.width(), widthPx / rasterScale())
                             + 2 * fringe);
    const qreal bandY =
            std::min(bounds.height() / 2,
                     std::max(shape.radius * shape.radiusScale.height(), widthPx / rasterScale())
                             + 2 * fringe);
    // One draw with disjoint strips avoids shading the transparent window interior.
    fillColorRects(
            {{bounds.x(), bounds.y(), bounds.width(), bandY},
             {bounds.x(), bounds.bottom() - bandY, bounds.width(), bandY},
             {bounds.x(), bounds.y() + bandY, bandX, bounds.height() - 2 * bandY},
             {bounds.right() - bandX, bounds.y() + bandY, bandX, bounds.height() - 2 * bandY}},
            color);
    m_outlineWidth = 0;
}

void EglRenderer::fillRect(const QRect &rect, const Color &color)
{
    fillRectF(rect, color);
}

void EglRenderer::fillRectF(const QRectF &rect, const Color &color)
{
    fillColorRects({rect}, color);
}

void EglRenderer::fillColorRects(std::initializer_list<QRectF> rects, const Color &color)
{
    if (!m_current)
        return;
    Q_ASSERT(rects.size() <= 4);
    float vertices[48] = {};
    int count = 0;
    for (const auto &rect : rects) {
        if (rect.isEmpty())
            continue;
        float quad[8];
        quadFor(rect.translated(-m_pass.offset()), quad);
        for (int index : {0, 1, 2, 2, 1, 3}) {
            vertices[count++] = quad[index * 2];
            vertices[count++] = quad[index * 2 + 1];
        }
    }
    if (!count)
        return;

    glUseProgram(m_colorProgram);
    applyRoundedClip(m_colorProgram);
    glUniform4f(m_colorProgram.uniform("uColor"), color.r * color.a, color.g * color.a,
                color.b * color.a, color.a);
    glVertexAttribPointer(m_colorProgram.attribute("aPosition"), 2, GL_FLOAT, GL_FALSE, 0,
                          vertices);
    glEnableVertexAttribArray(m_colorProgram.attribute("aPosition"));

    if (color.a < 1.0f || !m_roundedClips.empty() || m_outlineWidth > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    glDrawArrays(GL_TRIANGLES, 0, count / 2);

    glDisable(GL_BLEND);
    glDisableVertexAttribArray(m_colorProgram.attribute("aPosition"));
}

void EglRenderer::drawTexture(Texture *texture, const QRect &target, float alpha)
{
    drawTextureF(texture, target, alpha);
}

void EglRenderer::drawTextureF(Texture *texture, const QRectF &target, float alpha)
{
    drawTextureTransformed(texture, target, QTransform(), alpha);
}

void EglRenderer::drawTextureTransformed(Texture *texture, const QRectF &target,
                                         const QTransform &transform, float alpha)
{
    QTransform mapping;
    if (auto *gl = dynamic_cast<GlTexture *>(texture); gl && !gl->contentPixels.isEmpty()) {
        mapping.translate(gl->contentPixels.x() / gl->size().width(),
                          gl->contentPixels.y() / gl->size().height());
        mapping.scale(gl->contentPixels.width() / gl->size().width(),
                      gl->contentPixels.height() / gl->size().height());
    }
    drawTextureMappedF(texture, target, alpha, mapping, transform);
}

void EglRenderer::drawTextureMapped(Texture *texture, const QRect &target, float alpha,
                                    const QTransform &mapping)
{
    QRectF aligned = target;
    QTransform texelMapping = mapping;
    if (texture && rasterScale() > 0) {
        const QSize pixels = texture->size();
        const auto sourcePoint = [&](const QPointF &point) {
            const QPointF uv = mapping.map(point);
            return QPointF(uv.x() * pixels.width(), uv.y() * pixels.height());
        };
        const QPointF source = sourcePoint({0, 0});
        const QPointF horizontal = sourcePoint({1, 0}) - source;
        const QPointF vertical = sourcePoint({0, 1}) - source;
        const QSizeF sourceSize(std::hypot(horizontal.x(), horizontal.y()),
                                std::hypot(vertical.x(), vertical.y()));
        const QSize deviceSize(qRound(target.width() * rasterScale()),
                               qRound(target.height() * rasterScale()));
        const auto equalPixel = [](qreal a, qreal b) {
            return std::abs(a - b) < 0.001;
        };
        // A size the client chose in device pixels, like an Xwayland popup,
        // need not be a whole number of logical units: the rounded logical size
        // is then a pixel off. Texels still land one per pixel; the last row or
        // column is repeated from the texture edge, or left out.
        const auto nearPixel = [&](qreal a, qreal b) {
            return equalPixel(a, std::round(a)) && std::abs(a - b) < 1.001;
        };
        if (nearPixel(sourceSize.width(), deviceSize.width())
            && nearPixel(sourceSize.height(), deviceSize.height())
            && equalPixel(source.x(), std::round(source.x()))
            && equalPixel(source.y(), std::round(source.y()))) {
            if (!equalPixel(sourceSize.width(), deviceSize.width())
                || !equalPixel(sourceSize.height(), deviceSize.height()))
                texelMapping = QTransform::fromScale(deviceSize.width() / sourceSize.width(),
                                                     deviceSize.height() / sourceSize.height())
                        * mapping;
            // Like Exo's host origin adjustment, keep native-density texels on physical pixels.
            const QPointF outputOrigin = outputContext().logicalViewport.topLeft();
            const QPointF origin = (QPointF(target.topLeft()) - outputOrigin) * rasterScale();
            aligned = QRectF(outputOrigin
                                     + QPointF(std::round(origin.x()), std::round(origin.y()))
                                             / rasterScale(),
                             QSizeF(deviceSize) / rasterScale());
        }
    }
    drawTextureMappedF(texture, aligned, alpha, texelMapping);
}

void EglRenderer::drawTextureMappedF(Texture *texture, const QRectF &target, float alpha,
                                     const QTransform &mapping, const QTransform &geometry)
{
    auto *glTexture = dynamic_cast<GlTexture *>(texture);
    if (!m_current || !glTexture || target.isEmpty())
        return;

    float vertices[8] = {};
    const auto corner = [&](const QPointF &point) {
        return geometry.map(point) - m_pass.offset();
    };
    quadFor({corner(target.topLeft()), corner(target.topRight()), corner(target.bottomLeft()),
             corner(target.bottomRight())},
            vertices);

    // Matches the vertex order in quadFor(); the image origin is top left.
    float texCoords[8];
    for (int i = 0; i < 4; ++i) {
        const QPointF uv = mapping.map(QPointF(i % 2, i / 2));
        texCoords[2 * i] = uv.x();
        texCoords[2 * i + 1] = glTexture->inverted ? 1 - uv.y() : uv.y();
    }

    const bool external = glTexture->target() == GL_TEXTURE_EXTERNAL_OES;
    if (external && !m_externalProgram)
        return;

    const int positionAttr = external ? m_externalProgram.attribute("aPosition")
                                      : m_textureProgram.attribute("aPosition");
    const int coordAttr = external ? m_externalProgram.attribute("aTexCoord")
                                   : m_textureProgram.attribute("aTexCoord");

    glUseProgram(external ? m_externalProgram : m_textureProgram);
    applyRoundedClip(external ? m_externalProgram : m_textureProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(glTexture->target(), glTexture->name());
    glUniform1i(external ? m_externalProgram.uniform("uTexture")
                         : m_textureProgram.uniform("uTexture"),
                0);
    glUniform1f(external ? m_externalProgram.uniform("uAlpha") : m_textureProgram.uniform("uAlpha"),
                alpha);
    if (!external)
        glUniform1f(m_textureProgram.uniform("uSwizzle"),
                    pixelFormatIsBgr(glTexture->format()) ? 1.0f : 0.0f);
    const bool hasAlpha = pixelFormatHasAlpha(glTexture->format()) && !glTexture->contentOpaque;
    glUniform1f(external ? m_externalProgram.uniform("uHasAlpha")
                         : m_textureProgram.uniform("uHasAlpha"),
                hasAlpha ? 1.0f : 0.0f);

    glVertexAttribPointer(positionAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(positionAttr);
    glVertexAttribPointer(coordAttr, 2, GL_FLOAT, GL_FALSE, 0, texCoords);
    glEnableVertexAttribArray(coordAttr);

    // An XRGB buffer drawn whole shades to alpha 1; blending it only costs a read.
    const bool opaque = !hasAlpha && alpha >= 1
            && m_roundedClips.empty() && m_outlineWidth <= 0;
    if (m_pass.blendMode() == GlRenderPass::BlendMode::Copy || opaque) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
    glDisableVertexAttribArray(positionAttr);
    glDisableVertexAttribArray(coordAttr);
    glBindTexture(glTexture->target(), 0);
}

void EglRenderer::drawBackgroundBlur(const QRect &bounds, int radius, const QRect &clip,
                                     const GlassParams &glass, const QPainterPath &shape)
{
    if (!m_current || !m_backdrop)
        return;
    const double scale = rasterScale();
    const auto coverage = [scale](const QRect &rect) {
        return QRectF(QPointF(rect.topLeft()) * scale, QSizeF(rect.size()) * scale).toAlignedRect();
    };
    ScopedValue clips(m_roundedClips);
    for (auto &shape : m_roundedClips) {
        shape.bounds = QRectF(shape.bounds.topLeft() * scale, shape.bounds.size() * scale);
        shape.radius *= scale;
    }
    const auto sources = m_pass.backdropSources(scale, outputContext().transform);
    const QPointF offset = m_pass.offset();

    struct RestoreGeometry {
        EglRenderer &renderer;
        GlRenderPass &pass;
        QPointF offset;
        double scale;

        ~RestoreGeometry()
        {
            pass.setGeometry(pass.viewport(), offset);
            renderer.setRasterScale(scale);
        }
    } restore {*this, m_pass, offset, scale};

    m_pass.setGeometry(m_pass.viewport(), offset * scale);
    setRasterScale(1);
    m_backdrop->draw(
            *this, coverage(bounds), qRound(radius * scale), coverage(clip), glass, scale, shape,
            sources, m_roundedClips,
            [this](const QRectF &rect, float *vertices) {
                quadFor(rect.translated(-m_pass.offset()), vertices);
            },
            [this](const GlProgram &program) {
                applyRoundedClip(program);
            });
}

} // namespace cutefish::render
