// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wayland/protocols/image_capture.h"

#include <QObject>
#include <algorithm>
#include <cstring>
#include <ext-image-capture-source-v1-server-protocol.h>
#include <ext-image-copy-capture-v1-server-protocol.h>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <wayland-server-protocol.h>

#include "compositor/buffer.h"
#include "compositor/capture_source.h"
#include "compositor/dmabuf_buffer.h"
#include "compositor/shm_pool.h"
#include "core/logging.h"
#include "render/renderer.h"
#include "scene/damage_tracker.h"
#include "scene/node.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/protocols/wl_shm.h"

namespace cutefish::wayland {

namespace {

struct Session;
struct Frame;
} // namespace

struct ImageCaptureManager::State {
    Authorize authorize;
    Authorize bindable;
    std::function<bool()> captureAllowed;
    std::function<void()> repaint;
    ToplevelSourceResolver toplevelSource;
    const render::Renderer *renderer = nullptr;
    bool active = true;
    std::vector<std::weak_ptr<Session>> sessions;
    std::vector<std::shared_ptr<Frame>> pending;
};

namespace {

using State = ImageCaptureManager::State;
using Source = std::shared_ptr<compositor::CaptureSource>;

void destroy(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

bool writeImage(compositor::ShmBuffer &shm, const QImage &image)
{
    return !image.isNull() && image.size() == shm.size()
            && shm.pool()->writeRows(size_t(shm.offset()), size_t(shm.stride()),
                                     size_t(image.height()), size_t(image.width()) * 4,
                                     image.constBits(), size_t(image.bytesPerLine()));
}

struct Session : QObject {
    std::shared_ptr<State> state;
    Source source;
    wl_resource *resource = nullptr;
    Frame *frame = nullptr;
    bool cursors = false;
    bool stopped = false;
    // A session's first frame is composed on demand; after that a capture
    // waits for the next composition, so a static screen costs nothing.
    bool delivered = false;
    // What changed since this session's last frame, for the damage events.
    // Its history starts empty, so the first frame reports everything.
    scene::DamageTracker tracker;
    QRect lastCursor;

    void constraints()
    {
        if (stopped || !resource)
            return;
        if (!state->active || !source || !source->active) {
            stopped = true;
            ext_image_copy_capture_session_v1_send_stopped(resource);
            return;
        }
        const auto size = source->pixelSize();
        ext_image_copy_capture_session_v1_send_buffer_size(resource, size.width(), size.height());
        ext_image_copy_capture_session_v1_send_shm_format(resource, WL_SHM_FORMAT_ARGB8888);
        // DMA-BUF targets are offered only once the GPU has shown it can
        // render into such a buffer, so a client never learns of a format the
        // frame path would refuse.
        if (state->renderer) {
            const render::DmaBufFormats &formats = state->renderer->captureDmaBufFormats();
            const dev_t device = state->renderer->captureDmaBufDevice();
            if (!formats.empty() && device) {
                wl_array deviceArray;
                wl_array_init(&deviceArray);
                if (void *slot = wl_array_add(&deviceArray, sizeof(device)))
                    std::memcpy(slot, &device, sizeof(device));
                ext_image_copy_capture_session_v1_send_dmabuf_device(resource, &deviceArray);
                wl_array_release(&deviceArray);
                for (const auto &[format, modifiers] : formats) {
                    wl_array modifierArray;
                    wl_array_init(&modifierArray);
                    for (const uint64_t modifier : modifiers) {
                        if (void *slot = wl_array_add(&modifierArray, sizeof(modifier)))
                            std::memcpy(slot, &modifier, sizeof(modifier));
                    }
                    ext_image_copy_capture_session_v1_send_dmabuf_format(resource, format,
                                                                         &modifierArray);
                    wl_array_release(&modifierArray);
                }
            }
        }
        ext_image_copy_capture_session_v1_send_done(resource);
    }
};

struct Frame {
    std::shared_ptr<Session> session;
    wl_resource *resource;
    std::weak_ptr<compositor::Buffer> buffer;
    bool attached = false;
    bool captured = false;
    bool finished = false;
    uint64_t generation = 0;
    uint32_t transform = 0;
    QSize size;
    // Buffer-coordinate rectangles, sent before ready.
    std::vector<QRect> damage;
    // A shm capture still on its way back from the GPU, written into the
    // buffer when the frame is presented.
    std::shared_ptr<render::PendingImage> pixels;

    ~Frame()
    {
        auto &pending = session->state->pending;
        std::erase_if(pending, [this](const auto &frame) {
            return frame.get() == this;
        });
        if (session->frame == this)
            session->frame = nullptr;
    }

    bool mutableRequest()
    {
        if (!captured)
            return true;
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "frame already captured");
        return false;
    }

    void fail(uint32_t reason)
    {
        if (finished)
            return;
        finished = true;
        pixels.reset();
        std::erase_if(session->state->pending, [this](const auto &frame) {
            return frame.get() == this;
        });
        ext_image_copy_capture_frame_v1_send_failed(resource, reason);
        if (captured && session->state->repaint)
            session->state->repaint();
    }
};

void attach(wl_client *, wl_resource *resource, wl_resource *buffer)
{
    auto *frame = static_cast<std::shared_ptr<Frame> *>(wl_resource_get_user_data(resource))->get();
    if (frame->mutableRequest()) {
        frame->attached = true;
        frame->buffer = bufferFromResource(buffer);
    }
}

void damage(wl_client *, wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height)
{
    auto *frame = static_cast<std::shared_ptr<Frame> *>(wl_resource_get_user_data(resource))->get();
    if (!frame->mutableRequest())
        return;
    // Every frame is a complete paint, so the client's buffer damage only
    // needs validating.
    if (x < 0 || y < 0 || width <= 0 || height <= 0)
        wl_resource_post_error(resource,
                               EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
                               "invalid buffer damage");
}

void capture(wl_client *client, wl_resource *resource)
{
    auto *frame = static_cast<std::shared_ptr<Frame> *>(wl_resource_get_user_data(resource))->get();
    if (!frame->mutableRequest())
        return;
    if (!frame->attached) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
                               "capture needs a buffer");
        return;
    }
    frame->captured = true;
    const auto &session = frame->session;
    if (!session->state->active || !session->source || !session->source->active || session->stopped
        || !session->state->authorize(client)) {
        frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }
    session->state->pending.push_back(
            *static_cast<std::shared_ptr<Frame> *>(wl_resource_get_user_data(resource)));
    if (!session->delivered)
        session->state->repaint();
}

const struct ext_image_copy_capture_frame_v1_interface frameImplementation = {destroy, attach,
                                                                              damage, capture};

void createFrame(wl_client *client, wl_resource *resource, uint32_t id)
{
    auto session = *static_cast<std::shared_ptr<Session> *>(wl_resource_get_user_data(resource));
    if (session->frame) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
                               "destroy the previous frame first");
        return;
    }
    auto *result = wl_resource_create(client, &ext_image_copy_capture_frame_v1_interface, 1, id);
    if (!result) {
        wl_client_post_no_memory(client);
        return;
    }
    auto frame = std::make_shared<Frame>();
    frame->session = session;
    frame->resource = result;
    session->frame = frame.get();
    wl_resource_set_implementation(result, &frameImplementation, new std::shared_ptr<Frame>(frame),
                                   [](wl_resource *resource) {
                                       auto *owner = static_cast<std::shared_ptr<Frame> *>(
                                               wl_resource_get_user_data(resource));
                                       auto frame = *owner;
                                       // Only a frame still pending changes what
                                       // the next composition has to do.
                                       const bool pending = frame->captured && !frame->finished;
                                       frame->finished = true;
                                       frame->resource = nullptr;
                                       frame->session->frame = nullptr;
                                       std::erase(frame->session->state->pending, frame);
                                       if (pending && frame->session->state->repaint)
                                           frame->session->state->repaint();
                                       delete owner;
                                   });
}

const struct ext_image_copy_capture_session_v1_interface sessionImplementation = {createFrame,
                                                                                  destroy};

void makeSession(wl_client *client, uint32_t id, const std::shared_ptr<State> &state, Source source,
                 bool cursors)
{
    auto *resource =
            wl_resource_create(client, &ext_image_copy_capture_session_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    auto session = std::make_shared<Session>();
    session->state = state;
    session->source = std::move(source);
    session->resource = resource;
    session->cursors = cursors;
    wl_resource_set_implementation(resource, &sessionImplementation,
                                   new std::shared_ptr<Session>(session),
                                   [](wl_resource *resource) {
                                       auto *owner = static_cast<std::shared_ptr<Session> *>(
                                               wl_resource_get_user_data(resource));
                                       (*owner)->resource = nullptr;
                                       delete owner;
                                   });
    std::erase_if(state->sessions, [](const auto &session) {
        return session.expired();
    });
    state->sessions.push_back(session);
    if (session->source) {
        QObject::connect(
                session->source.get(), &compositor::CaptureSource::changed, session.get(),
                [weak = std::weak_ptr<Session>(session)] {
                    if (auto session = weak.lock()) {
                        session->constraints();
                        if (!session->source->active && session->frame && session->frame->captured)
                            session->frame->fail(
                                    EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
                    }
                });
    }
    session->constraints();
}

const struct ext_image_capture_source_v1_interface sourceImplementation = {destroy};

void createSession(wl_client *client, wl_resource *resource, uint32_t id, wl_resource *source,
                   uint32_t options)
{
    if (options & ~EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) {
        wl_resource_post_error(resource, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
                               "unknown capture option");
        return;
    }
    auto state = *static_cast<std::shared_ptr<State> *>(wl_resource_get_user_data(resource));
    Source target;
    if (state->authorize(client)
        && wl_resource_instance_of(source, &ext_image_capture_source_v1_interface,
                                   &sourceImplementation))
        target = *static_cast<Source *>(wl_resource_get_user_data(source));
    makeSession(client, id, state, target, options != 0);
}

struct CursorSession {
    std::shared_ptr<State> state;
    bool requested = false;
};

void getCursorSession(wl_client *client, wl_resource *resource, uint32_t id)
{
    auto *cursor = static_cast<CursorSession *>(wl_resource_get_user_data(resource));
    if (std::exchange(cursor->requested, true)) {
        wl_resource_post_error(resource,
                               EXT_IMAGE_COPY_CAPTURE_CURSOR_SESSION_V1_ERROR_DUPLICATE_SESSION,
                               "cursor capture session already requested");
        return;
    }
    // Cursor-only sources are unavailable; the base session reports stopped.
    makeSession(client, id, cursor->state, {}, false);
}

const struct ext_image_copy_capture_cursor_session_v1_interface cursorImplementation = {
        destroy, getCursorSession};

void createCursorSession(wl_client *client, wl_resource *resource, uint32_t id, wl_resource *,
                         wl_resource *)
{
    auto *result =
            wl_resource_create(client, &ext_image_copy_capture_cursor_session_v1_interface, 1, id);
    if (!result) {
        wl_client_post_no_memory(client);
        return;
    }
    auto state = *static_cast<std::shared_ptr<State> *>(wl_resource_get_user_data(resource));
    wl_resource_set_implementation(
            result, &cursorImplementation, new CursorSession {state}, [](wl_resource *resource) {
                delete static_cast<CursorSession *>(wl_resource_get_user_data(resource));
            });
}

const struct ext_image_copy_capture_manager_v1_interface copyImplementation = {
        createSession, createCursorSession, destroy};

void exportSource(wl_client *client, uint32_t id, Source source)
{
    auto *result = wl_resource_create(client, &ext_image_capture_source_v1_interface, 1, id);
    if (!result) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(
            result, &sourceImplementation, new Source(std::move(source)),
            [](wl_resource *resource) {
                delete static_cast<Source *>(wl_resource_get_user_data(resource));
            });
}

void createSource(wl_client *client, wl_resource *resource, uint32_t id, wl_resource *output)
{
    auto state = *static_cast<std::shared_ptr<State> *>(wl_resource_get_user_data(resource));
    exportSource(client, id, state->authorize(client) ? outputCaptureSource(output) : Source());
}

const struct ext_output_image_capture_source_manager_v1_interface outputImplementation = {
        createSource, destroy};

void createToplevelSource(wl_client *client, wl_resource *resource, uint32_t id,
                          wl_resource *handle)
{
    auto state = *static_cast<std::shared_ptr<State> *>(wl_resource_get_user_data(resource));
    Source source;
    if (state->authorize(client) && state->toplevelSource)
        source = state->toplevelSource(handle);
    exportSource(client, id, std::move(source));
}

const struct ext_foreign_toplevel_image_capture_source_manager_v1_interface
        toplevelImplementation = {createToplevelSource, destroy};

void bind(wl_client *client, void *data, uint32_t, uint32_t id, const wl_interface *interface,
          const void *implementation)
{
    auto state = *static_cast<std::shared_ptr<State> *>(data);
    if (!state->bindable(client)) {
        wl_client_post_implementation_error(client, "capture access denied");
        return;
    }
    auto *resource = wl_resource_create(client, interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(
            resource, implementation, new std::shared_ptr<State>(state), [](wl_resource *resource) {
                delete static_cast<std::shared_ptr<State> *>(wl_resource_get_user_data(resource));
            });
}

// Maps damage from global logical coordinates into a frame buffer whose
// content sits at `content` (native pixels) and covers `viewport` at `scale`.
// A rotated output reports its whole buffer: the mapping through the
// transform is not worth a second code path for the rare rotated share.
std::vector<QRect> bufferDamage(const QRegion &logical, const QRect &viewport, double scale,
                                uint32_t transform, const QRect &content, const QSize &size)
{
    const QRect whole(QPoint(), size);
    // QRegion::contains(QRect) means overlap; whole coverage is an equality.
    if (transform != 0 || viewport.isEmpty()
        || logical.intersected(viewport) == QRegion(viewport))
        return {whole};
    std::vector<QRect> rects;
    for (const QRect &rect : logical.intersected(viewport)) {
        const QRectF moved(QPointF(rect.topLeft() - viewport.topLeft()) * scale,
                           QSizeF(rect.size()) * scale);
        const QRect pixels = moved.toAlignedRect().translated(content.topLeft()).intersected(whole);
        if (!pixels.isEmpty())
            rects.push_back(pixels);
    }
    return rects;
}

void deliver(State &state, const std::shared_ptr<Frame> &frame, uint64_t monotonicNs)
{
    const uint64_t seconds = monotonicNs / 1000000000;
    ext_image_copy_capture_frame_v1_send_transform(frame->resource, frame->transform);
    for (const QRect &rect : frame->damage) {
        ext_image_copy_capture_frame_v1_send_damage(frame->resource, rect.x(), rect.y(),
                                                    rect.width(), rect.height());
    }
    ext_image_copy_capture_frame_v1_send_presentation_time(
            frame->resource, seconds >> 32, uint32_t(seconds), monotonicNs % 1000000000);
    frame->finished = true;
    frame->session->delivered = true;
    std::erase(state.pending, frame);
    ext_image_copy_capture_frame_v1_send_ready(frame->resource);
    // A static fullscreen client needs another frame to resume scanout.
    if (state.repaint)
        state.repaint();
}
} // namespace

void ImageCaptureManager::setCaptureAllowed(std::function<bool()> allowed)
{
    m_state->captureAllowed = std::move(allowed);
}

void ImageCaptureManager::failPending()
{
    const auto pending = m_state->pending;
    for (const auto &frame : pending) {
        if (!frame->finished)
            frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
    }
}

void ImageCaptureManager::setToplevelSourceResolver(ToplevelSourceResolver resolver)
{
    m_state->toplevelSource = std::move(resolver);
}

void ImageCaptureManager::setRenderer(const render::Renderer *renderer)
{
    m_state->renderer = renderer;
}

ImageCaptureManager::ImageCaptureManager(wl_display *display, Authorize authorize,
                                         std::function<void()> repaint, Authorize bindable)
    : m_state(std::make_shared<State>())
{
    m_state->authorize = std::move(authorize);
    m_state->bindable = bindable ? std::move(bindable) : m_state->authorize;
    m_state->repaint = std::move(repaint);
    m_sourceGlobal = wl_global_create(
            display, &ext_output_image_capture_source_manager_v1_interface, 1, &m_state,
            [](wl_client *client, void *data, uint32_t version, uint32_t id) {
                bind(client, data, version, id,
                     &ext_output_image_capture_source_manager_v1_interface, &outputImplementation);
            });
    m_toplevelSourceGlobal = wl_global_create(
            display, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1, &m_state,
            [](wl_client *client, void *data, uint32_t version, uint32_t id) {
                bind(client, data, version, id,
                     &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
                     &toplevelImplementation);
            });
    m_copyGlobal = wl_global_create(
            display, &ext_image_copy_capture_manager_v1_interface, 1, &m_state,
            [](wl_client *client, void *data, uint32_t version, uint32_t id) {
                bind(client, data, version, id, &ext_image_copy_capture_manager_v1_interface,
                     &copyImplementation);
            });
}

ImageCaptureManager::~ImageCaptureManager()
{
    shutdown();
}

void ImageCaptureManager::shutdown()
{
    m_state->active = false;
    m_state->authorize = [](const wl_client *) {
        return false;
    };
    m_state->bindable = m_state->authorize;
    m_state->repaint = {};
    m_state->toplevelSource = {};
    m_state->renderer = nullptr;
    auto pending = std::move(m_state->pending);
    for (const auto &frame : pending)
        frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
    for (auto &weak : m_state->sessions)
        if (auto session = weak.lock())
            session->constraints();
    if (m_sourceGlobal)
        wl_global_destroy(std::exchange(m_sourceGlobal, nullptr));
    if (m_toplevelSourceGlobal)
        wl_global_destroy(std::exchange(m_toplevelSourceGlobal, nullptr));
    if (m_copyGlobal)
        wl_global_destroy(std::exchange(m_copyGlobal, nullptr));
}

wl_global *ImageCaptureManager::sourceGlobal() const
{
    return m_sourceGlobal;
}

wl_global *ImageCaptureManager::toplevelSourceGlobal() const
{
    return m_toplevelSourceGlobal;
}

wl_global *ImageCaptureManager::copyGlobal() const
{
    return m_copyGlobal;
}

bool ImageCaptureManager::hasPending(const QString &output) const
{
    return std::any_of(m_state->pending.begin(), m_state->pending.end(), [&](const auto &frame) {
        return !frame->finished && frame->session->source->renderedOn() == output;
    });
}

void ImageCaptureManager::renderPending(render::Renderer &renderer,
                                        const std::function<void(bool)> &paint)
{
    Painters painters;
    painters.scene = paint;
    renderPending(renderer, painters);
}

void ImageCaptureManager::renderPending(render::Renderer &renderer, const Painters &painters)
{
    const auto state = m_state;
    const auto pending = state->pending;
    const render::OutputRenderContext outputContext = renderer.outputContext();
    struct Capture {
        std::shared_ptr<render::PendingImage> later;
        QImage image;
    };
    Capture captures[2];
    const bool allowed = !state->captureAllowed || state->captureAllowed();
    const QRect cursorBounds = painters.cursorBounds ? painters.cursorBounds() : QRect();
    for (const auto &frame : pending) {
        if (frame->finished || frame->generation)
            continue;
        const auto &session = frame->session;
        const auto &source = session->source;
        if (source->renderedOn() != outputContext.outputId)
            continue;
        // Left pending: the next composition after the unlock captures it.
        if (!allowed)
            continue;
        if (!state->authorize(wl_resource_get_client(frame->resource))) {
            frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
            continue;
        }
        const bool window = source->kind == compositor::CaptureSource::Kind::Window;
        const QSize size = source->pixelSize();
        auto buffer = frame->buffer.lock();
        auto shm = std::dynamic_pointer_cast<compositor::ShmBuffer>(buffer);
        auto dmabuf = std::dynamic_pointer_cast<compositor::DmaBufBuffer>(buffer);
        const bool sizeOk = buffer && buffer->size() == size
                && (window || size == outputContext.nativeTargetSize);
        const bool formatOk = (shm && shm->format() == render::PixelFormat::Bgra8888)
                || (dmabuf && renderer.captureDmaBufFormats().contains(dmabuf->attributes().format));
        if (!sizeOk || !formatOk) {
            frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
            continue;
        }
        if (window && !source->node) {
            frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
            continue;
        }
        // A minimized window paints nothing; the frame waits for it to come
        // back, so the stream holds its last picture instead of going blank.
        if (window && !source->node->isVisible())
            continue;

        // The frame's own context: the display's for an output, the window's
        // bounds at its own scale for a window.
        render::OutputRenderContext context = outputContext;
        QRect viewport = outputContext.logicalViewport;
        QRect content = outputContext.nativeContentRect.isEmpty()
                ? QRect(QPoint(), size)
                : outputContext.nativeContentRect;
        if (window) {
            context.logicalViewport = source->bounds;
            context.nativeTargetSize = size;
            context.scale = source->scale;
            context.transform = 0;
            context.nativeContentRect = QRect(QPoint(), size);
            viewport = source->bounds;
            content = QRect(QPoint(), size);
        }

        // Damage since this session's previous frame, tracked before the paint
        // so the tracker's history is this composition's.
        QRegion logical;
        const scene::Node *root = window ? source->node : painters.root;
        if (root)
            logical = session->tracker.update(*root, viewport, context.scale, context.transform);
        else
            logical = viewport;
        if (session->cursors && cursorBounds != session->lastCursor) {
            logical += cursorBounds;
            logical += session->lastCursor;
        }
        if (session->cursors)
            session->lastCursor = cursorBounds;

        const auto paint = [&] {
            if (window) {
                source->node->render(renderer);
                if (session->cursors && painters.cursors)
                    painters.cursors();
            } else if (painters.scene) {
                const Composited composited =
                        painters.composited ? painters.composited() : Composited::Unusable;
                const bool reusable = composited == Composited::WithoutCursor
                        || (composited == Composited::WithCursor && session->cursors);
                if (!reusable || !renderer.copyOutputFrame(size)) {
                    painters.scene(session->cursors);
                } else if (session->cursors && composited == Composited::WithoutCursor
                           && painters.cursors) {
                    painters.cursors();
                }
            }
        };
        bool copied = false;
        if (dmabuf) {
            copied = renderer.captureToDmaBuf(context, size, dmabuf->attributes(), paint);
        } else {
            Capture capture;
            if (window) {
                capture.later = renderer.captureImageLater(context, size, paint);
                if (!capture.later)
                    capture.image = renderer.captureImage(context, size, paint);
            } else {
                // Output sessions of the same output share one paint.
                auto &cached = captures[session->cursors ? 1 : 0];
                if (!cached.later && cached.image.isNull()) {
                    cached.later = renderer.captureFrameLater(paint);
                    if (!cached.later)
                        cached.image = renderer.captureFrame(paint);
                }
                capture = cached;
            }
            if (frame->finished)
                continue;
            if (capture.later) {
                // Presentation comes after the GPU finished this read, so
                // collecting it then does not stall the compositor.
                frame->pixels = std::move(capture.later);
                copied = true;
            } else {
                copied = writeImage(*shm, capture.image);
            }
        }
        if (frame->finished)
            continue;
        if (!copied) {
            if (dmabuf) {
                qCWarning(cfWayland, "Capture into a DMA-BUF failed (format 0x%08x, modifier 0x%llx)",
                          dmabuf->attributes().format,
                          static_cast<unsigned long long>(dmabuf->attributes().modifier));
            }
            frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
            continue;
        }
        frame->generation = outputContext.frame;
        frame->transform = context.transform;
        frame->size = size;
        frame->damage = bufferDamage(logical, viewport, context.scale, context.transform, content,
                                     size);
    }
    std::erase_if(state->sessions, [](const auto &session) {
        return session.expired();
    });
}

void ImageCaptureManager::drawn(uint64_t generation, uint64_t aimedNs)
{
    const auto pending = m_state->pending;
    for (const auto &frame : pending) {
        if (generation && aimedNs && frame->generation == generation && !frame->pixels)
            deliver(*m_state, frame, aimedNs);
    }
}

void ImageCaptureManager::presented(uint64_t generation, uint64_t monotonicNs)
{
    const auto pending = m_state->pending;
    for (const auto &frame : pending) {
        if (!generation || frame->generation != generation)
            continue;
        if (!monotonicNs) {
            frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
            continue;
        }
        if (frame->pixels) {
            const auto shm =
                    std::dynamic_pointer_cast<compositor::ShmBuffer>(frame->buffer.lock());
            const QImage image = std::exchange(frame->pixels, nullptr)->take();
            if (!shm || !writeImage(*shm, image)) {
                frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
                continue;
            }
        }
        deliver(*m_state, frame, monotonicNs);
    }
}

void ImageCaptureManager::cursorChanged()
{
    if (!m_state->repaint)
        return;
    for (const auto &frame : m_state->pending) {
        if (!frame->finished && !frame->generation && frame->session->cursors) {
            m_state->repaint();
            return;
        }
    }
}

void ImageCaptureManager::discarded(uint64_t generation)
{
    const auto pending = m_state->pending;
    for (const auto &frame : pending)
        if (generation && frame->generation == generation)
            frame->fail(EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
}

} // namespace cutefish::wayland
