// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl_render_pass.h"

#include <GLES3/gl3.h>
// Extension declarations require the core GL types first.
#include <GLES2/gl2ext.h>
#include <cmath>
#include <cstring>
#include <utility>

#include "render/gl_program.h"

namespace cutefish::render {
namespace {

class ScopedGlState
{
public:
    explicit ScopedGlState(const EglContext &context)
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        es3 = context.glVersion().startsWith("OpenGL ES 3");
        external = context.supportsGl("GL_OES_EGL_image_external");
        if (es3) {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        scissored = glIsEnabled(GL_SCISSOR_TEST);
        blended = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (int i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
            if (external)
                glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &externalTextures[i]);
        }
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        // Only the attributes the renderer's programs are linked with; the
        // other slots are never touched.
        attributes.resize(GlProgram::kAttributeCount);
        for (GLuint i = 0; i < attributes.size(); ++i) {
            auto &a = attributes[i];
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
            if (es3)
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &a.integer);
            glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
    }

    ~ScopedGlState()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        if (es3) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
        }
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        scissored ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
        blended ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
        glBlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
        glUseProgram(program);
        for (int i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            if (external)
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTextures[i]);
        }
        glActiveTexture(active);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        for (GLuint i = 0; i < attributes.size(); ++i) {
            const auto &a = attributes[i];
            glBindBuffer(GL_ARRAY_BUFFER, a.buffer);
            if (a.integer)
                glVertexAttribIPointer(i, a.size, a.type, a.stride, a.pointer);
            else
                glVertexAttribPointer(i, a.size, a.type, a.normalized, a.stride, a.pointer);
            if (a.enabled)
                glEnableVertexAttribArray(i);
            else
                glDisableVertexAttribArray(i);
        }
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
    }

    ScopedGlState(const ScopedGlState &) = delete;
    ScopedGlState &operator=(const ScopedGlState &) = delete;

private:
    GLint framebuffer, viewport[4], scissor[4], blend[4], program, active, textures[2], pack,
            unpack;
    GLfloat clear[4];
    bool scissored, blended;
    bool es3, external;
    GLint readFramebuffer = 0, packBuffer = 0, unpackBuffer = 0, arrayBuffer = 0;
    GLint externalTextures[2] = {};

    struct Attribute {
        GLint enabled, size, type, normalized, stride, buffer;
        GLint integer = 0;
        void *pointer;
    };

    std::vector<Attribute> attributes;
};

class Framebuffer
{
public:
    explicit Framebuffer(GLuint texture)
    {
        glGenFramebuffers(1, &m_name);
        glBindFramebuffer(GL_FRAMEBUFFER, m_name);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    }

    ~Framebuffer() { glDeleteFramebuffers(1, &m_name); }

    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;

    bool complete() const
    {
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

private:
    GLuint m_name = 0;
};

class RendererState
{
public:
    explicit RendererState(Renderer &renderer)
        : m_renderer(renderer)
        , m_context(renderer.outputContext())
        , m_scale(renderer.rasterScale())
        , m_submission(renderer.presentationSubmission)
        , m_capturing(renderer.capturing)
    {
        renderer.presentationSubmission = nullptr;
    }

    ~RendererState()
    {
        m_renderer.Renderer::setOutputContext(m_context);
        m_renderer.setRasterScale(m_scale);
        m_renderer.presentationSubmission = m_submission;
        m_renderer.capturing = m_capturing;
    }

    RendererState(const RendererState &) = delete;
    RendererState &operator=(const RendererState &) = delete;

private:
    Renderer &m_renderer;
    OutputRenderContext m_context;
    double m_scale;
    PresentationSubmission *m_submission;
    bool m_capturing;
};

std::unique_ptr<GlTexture> allocate(const std::shared_ptr<EglContext> &context, const QSize &size,
                                    GLint filter)
{
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    return std::make_unique<GlTexture>(name, size, PixelFormat::Rgba8888, GL_TEXTURE_2D, context);
}

class GlPendingImage : public PendingImage
{
public:
    GlPendingImage(std::shared_ptr<EglContext> context, GLuint buffer, const QSize &size)
        : m_context(std::move(context))
        , m_buffer(buffer)
        , m_size(size)
    {
    }

    ~GlPendingImage() override
    {
        ScopedEglCurrent current(*m_context);
        if (current.valid())
            glDeleteBuffers(1, &m_buffer);
    }

    QImage take() override
    {
        // Output sessions of one output share the read.
        if (m_taken)
            return m_image;
        m_taken = true;
        ScopedEglCurrent current(*m_context);
        if (!current.valid())
            return {};
        GLint previous = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer);
        const qsizetype bytes = qsizetype(m_size.width()) * m_size.height() * 4;
        QImage image(m_size, QImage::Format_RGBA8888_Premultiplied);
        const void *pixels = image.isNull()
                ? nullptr
                : glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (pixels) {
            std::memcpy(image.bits(), pixels, size_t(bytes));
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, previous);
        if (pixels)
            m_image = image.mirrored(false, true)
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        return m_image;
    }

private:
    std::shared_ptr<EglContext> m_context;
    GLuint m_buffer;
    QSize m_size;
    bool m_taken = false;
    QImage m_image;
};

} // namespace

QRect BackdropSource::framebufferRect(const QRect &rect) const
{
    const QSize layout = oriented();
    const QRect r = rect.translated(-offset.toPoint());
    QRect turned = r;
    switch (transform) {
    case 1:
        turned = QRect(r.y(), layout.width() - r.x() - r.width(), r.height(), r.width());
        break;
    case 2:
        turned = QRect(layout.width() - r.x() - r.width(), layout.height() - r.y() - r.height(),
                       r.width(), r.height());
        break;
    case 3:
        turned = QRect(layout.height() - r.y() - r.height(), r.x(), r.height(), r.width());
        break;
    }
    return QRect(turned.x(), size.height() - turned.y() - turned.height(), turned.width(),
                 turned.height());
}

std::vector<BackdropSource> GlRenderPass::backdropSources(double scale, uint32_t transform) const
{
    auto sources = m_sources;
    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    sources.push_back({static_cast<unsigned int>(framebuffer), m_viewport, m_offset, transform});
    for (auto &source : sources)
        source.offset *= scale;
    return sources;
}

QImage GlRenderPass::capture(Renderer &renderer, const QSize &size,
                             const std::function<void()> &paint)
{
    return captureImage(renderer, renderer.outputContext(), size, paint);
}

std::unique_ptr<GlTexture> GlRenderPass::captureTexture(Renderer &renderer,
                                                        const OutputRenderContext &context,
                                                        const QSize &size,
                                                        const std::function<void()> &paint)
{
    if (size.isEmpty())
        return nullptr;
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (size.width() > limit || size.height() > limit)
        return nullptr;
    auto texture = allocate(m_context, size, GL_NEAREST);
    Framebuffer framebuffer(texture->name());
    if (!framebuffer.complete())
        return nullptr;
    ScopedValue viewport(m_viewport);
    ScopedValue offset(m_offset);
    renderer.Renderer::setOutputContext(context);
    // Letterboxed content keeps its place in the buffer, as on the display.
    const QRect content = context.nativeContentRect.isEmpty() ? QRect(QPoint(), size)
                                                              : context.nativeContentRect;
    m_viewport = content.size();
    m_offset = context.logicalViewport.topLeft();
    glViewport(content.x(), size.height() - content.y() - content.height(), content.width(),
               content.height());
    glDisable(GL_SCISSOR_TEST);
    renderer.capturing = true;
    renderer.clear({0, 0, 0, 0});
    paint();
    // Rows are bottom-up, as with every render into a texture here.
    texture->inverted = true;
    return texture;
}

QImage GlRenderPass::captureImage(Renderer &renderer, const OutputRenderContext &context,
                                  const QSize &size, const std::function<void()> &paint)
{
    ScopedGlState state(*m_context);
    RendererState rendererState(renderer);
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull())
        return {};
    auto texture = captureTexture(renderer, context, size, paint);
    if (!texture)
        return {};
    Framebuffer framebuffer(texture->name());
    if (!framebuffer.complete())
        return {};
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    if (glGetError() != GL_NO_ERROR)
        return {};
    return image.mirrored(false, true).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

std::shared_ptr<PendingImage> GlRenderPass::readBack(Renderer &renderer,
                                                     const OutputRenderContext &context,
                                                     const QSize &size,
                                                     const std::function<void()> &paint)
{
    if (!m_context->glVersion().startsWith("OpenGL ES 3"))
        return nullptr;
    ScopedGlState state(*m_context);
    RendererState rendererState(renderer);
    auto texture = captureTexture(renderer, context, size, paint);
    if (!texture)
        return nullptr;
    Framebuffer framebuffer(texture->name());
    if (!framebuffer.complete())
        return nullptr;
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    auto pending = std::make_shared<GlPendingImage>(m_context, buffer, size);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(size.width()) * size.height() * 4, nullptr,
                 GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    // Into the buffer: returns once queued, not once the GPU is done.
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (glGetError() != GL_NO_ERROR)
        return nullptr;
    return pending;
}

// The capture texture is drawn flipped into a texture bound to the DMA-BUF's
// EGLImage, the way viz copies a drawn render pass out with a flipped scaler
// pass instead of reading pixels back (GLRenderer::CopyDrawnRenderPass), and
// the image is bound as a plain GL_TEXTURE_2D render target as ui::
// NativePixmapEGLBinding does for RGB pixmaps.
bool GlRenderPass::captureToImage(Renderer &renderer, const OutputRenderContext &context,
                                  const QSize &size, EGLImageKHR image,
                                  const std::function<void()> &paint)
{
    ScopedGlState state(*m_context);
    RendererState rendererState(renderer);
    auto imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!imageTargetTexture || image == EGL_NO_IMAGE_KHR)
        return false;
    auto texture = captureTexture(renderer, context, size, paint);
    if (!texture)
        return false;

    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    imageTargetTexture(GL_TEXTURE_2D, image);
    glBindTexture(GL_TEXTURE_2D, 0);
    bool complete = glGetError() == GL_NO_ERROR;
    if (complete) {
        Framebuffer framebuffer(name);
        complete = framebuffer.complete();
        if (complete) {
            // A plain identity pass. Sampling the bottom-up capture as if it
            // were top-down lands it top-down in the target's memory.
            OutputRenderContext flat;
            flat.outputId = context.outputId;
            flat.logicalViewport = QRect(QPoint(), size);
            flat.nativeTargetSize = size;
            flat.nativeContentRect = QRect(QPoint(), size);
            ScopedValue viewport(m_viewport);
            ScopedValue offset(m_offset);
            ScopedValue blend(m_blendMode);
            renderer.Renderer::setOutputContext(flat);
            m_viewport = size;
            m_offset = QPointF();
            // The client reuses its buffers, so every pixel has to be written.
            m_blendMode = BlendMode::Copy;
            glViewport(0, 0, size.width(), size.height());
            glDisable(GL_SCISSOR_TEST);
            texture->inverted = false;
            renderer.drawTexture(texture.get(), QRect(QPoint(), size));
            glFlush();
        }
    }
    glDeleteTextures(1, &name);
    return complete;
}

QImage GlRenderPass::renderToImage(Renderer &renderer, const QSize &size,
                                   const std::function<void()> &paint)
{
    RendererState rendererState(renderer);
    if (size.isEmpty())
        return {};
    ScopedGlState state(*m_context);
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (size.width() > limit || size.height() > limit)
        return {};
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull())
        return {};
    auto texture = allocate(m_context, size, GL_NEAREST);
    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    Framebuffer framebuffer(texture->name());
    if (!framebuffer.complete())
        return {};
    ScopedValue sources(m_sources);
    ScopedValue viewport(m_viewport);
    ScopedValue offset(m_offset);
    m_sources.push_back({static_cast<unsigned int>(previous), m_viewport, m_offset,
                         renderer.outputContext().transform});
    auto context = renderer.outputContext();
    context.transform = 0;
    context.scale = 1;
    context.logicalViewport = QRect(QPoint(), size);
    context.nativeContentRect = QRect(QPoint(), size);
    renderer.Renderer::setOutputContext(context);
    m_viewport = size;
    m_offset = QPointF();
    glViewport(0, 0, size.width(), size.height());
    glDisable(GL_SCISSOR_TEST);
    renderer.clear({0, 0, 0, 0});
    paint();
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    if (glGetError() != GL_NO_ERROR)
        return {};
    return image.mirrored(false, true).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

bool GlRenderPass::render(Renderer &renderer, std::unique_ptr<Texture> &texture,
                          const QRect &bounds, const std::function<void()> &paint,
                          const QRect &repaint)
{
    RendererState rendererState(renderer);
    if (bounds.isEmpty())
        return false;
    ScopedGlState state(*m_context);
    const QRectF deviceBounds(QPointF(bounds.topLeft()) * renderer.rasterScale(),
                              QSizeF(bounds.size()) * renderer.rasterScale());
    const QRect coverage = deviceBounds.toAlignedRect();
    // Rounded outward, the coverage is a pixel wider or taller depending on
    // where its fractional origin falls, so at a fractional scale a moving
    // window would take a new texture nearly every frame. The texture is sized
    // for the largest coverage the bounds can round out to and keeps its size
    // as the origin moves; contentPixels says where the content is in it, and
    // the spare row and column stay clear.
    const bool fractional = renderer.rasterScale() != std::floor(renderer.rasterScale());
    const QSize allocation(int(std::ceil(bounds.width() * renderer.rasterScale())) + fractional,
                           int(std::ceil(bounds.height() * renderer.rasterScale())) + fractional);
    Q_ASSERT(allocation.width() >= coverage.width() && allocation.height() >= coverage.height());
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (allocation.width() > limit || allocation.height() > limit)
        return false;
    auto *target = dynamic_cast<GlTexture *>(texture.get());
    // Only a texture of this size still holds what lies outside `repaint`.
    QRect scissor;
    if (!target || target->size() != allocation) {
        texture = allocate(m_context, allocation, GL_LINEAR);
        target = static_cast<GlTexture *>(texture.get());
        target->inverted = true;
    } else if (!repaint.isEmpty()) {
        scissor = QRectF(QPointF(repaint.topLeft()) * renderer.rasterScale(),
                         QSizeF(repaint.size()) * renderer.rasterScale())
                          .toAlignedRect()
                          .translated(-coverage.topLeft())
                          .intersected(QRect(QPoint(), allocation));
    }
    if (target->mipmapped) {
        // The smaller levels would keep showing the previous contents.
        glBindTexture(GL_TEXTURE_2D, target->name());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        target->mipmapped = false;
    }
    target->contentPixels = deviceBounds.translated(-coverage.topLeft());
    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    // Every window and blur pass runs through here each frame, so framebuffer
    // objects are kept and re-pointed rather than created and destroyed per
    // pass: one per nesting depth, since a blur inside a window's pass copies
    // out of the framebuffer the window is being drawn into.
    const size_t depth = m_sources.size();
    while (m_framebuffers.size() <= depth) {
        GLuint name = 0;
        glGenFramebuffers(1, &name);
        m_framebuffers.push_back(name);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffers[depth]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->name(), 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    ScopedValue sources(m_sources);
    ScopedValue viewport(m_viewport);
    ScopedValue offset(m_offset);
    m_sources.push_back({static_cast<unsigned int>(previous), m_viewport, m_offset,
                         renderer.outputContext().transform});
    auto context = renderer.outputContext();
    context.transform = 0;
    context.scale = renderer.rasterScale();
    renderer.Renderer::setOutputContext(context);
    m_viewport = allocation;
    m_offset = QPointF(coverage.topLeft()) / renderer.rasterScale();
    glViewport(0, 0, allocation.width(), allocation.height());
    if (scissor.isEmpty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        // Rows are bottom-up in the texture.
        glEnable(GL_SCISSOR_TEST);
        glScissor(scissor.x(), allocation.height() - scissor.y() - scissor.height(),
                  scissor.width(), scissor.height());
    }
    renderer.clear({0, 0, 0, 0});
    paint();
    return true;
}

void GlRenderPass::releaseResources()
{
    if (!m_framebuffers.empty())
        glDeleteFramebuffers(GLsizei(m_framebuffers.size()), m_framebuffers.data());
    m_framebuffers.clear();
}

void GlRenderPass::generateMipmaps(Texture *texture)
{
    auto *target = dynamic_cast<GlTexture *>(texture);
    if (!target || target->mipmapped || target->target() != GL_TEXTURE_2D)
        return;
    // GLES 2 only mipmaps power-of-two sizes.
    if (!m_context->glVersion().startsWith("OpenGL ES 3")
        && !m_context->supportsGl("GL_OES_texture_npot"))
        return;
    ScopedGlState state(*m_context);
    glBindTexture(GL_TEXTURE_2D, target->name());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    target->mipmapped = glGetError() == GL_NO_ERROR;
    if (!target->mipmapped)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void GlRenderPass::limitMipmapLevel(Texture *texture, int level)
{
    auto *target = dynamic_cast<GlTexture *>(texture);
    // GLES 2 has no LOD range.
    if (!target || !target->mipmapped || !m_context->glVersion().startsWith("OpenGL ES 3"))
        return;
    ScopedGlState state(*m_context);
    glBindTexture(GL_TEXTURE_2D, target->name());
    // 1000 is GL's initial GL_TEXTURE_MAX_LOD.
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, level < 0 ? 1000.0f : float(level));
}

} // namespace cutefish::render
