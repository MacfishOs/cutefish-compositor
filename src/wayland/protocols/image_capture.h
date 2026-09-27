// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QObject>
#include <QRect>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>

struct wl_client;
struct wl_display;
struct wl_global;
struct wl_resource;

namespace cutefish::compositor {
class CaptureSource;
}

namespace cutefish::render {
class Renderer;
}

namespace cutefish::scene {
class Node;
}

namespace cutefish::wayland {

class ImageCaptureManager : public QObject
{
public:
    using Authorize = std::function<bool(const wl_client *)>;
    using ToplevelSourceResolver =
            std::function<std::shared_ptr<compositor::CaptureSource>(wl_resource *handle)>;

    // What a capture frame is painted from. `scene` paints the whole output
    // for output sources; a window source paints its own node and adds the
    // cursor layers through `cursors`. `cursorBounds` is the cursor image's
    // global logical rectangle (empty while hidden), which is damage a
    // hardware cursor never puts into the tree; `root` is the tree damage is
    // tracked against for output sources.
    struct Painters {
        std::function<void(bool cursors)> scene;
        std::function<void()> cursors;
        std::function<QRect()> cursorBounds;
        const scene::Node *root = nullptr;
    };

    ImageCaptureManager(wl_display *display, Authorize authorize, std::function<void()> repaint);
    ~ImageCaptureManager();

    // Releases the globals; must run before the display is destroyed.
    void shutdown();

    // Whether capture of the session is permitted right now. While a session
    // is locked no frame is rendered at all, rather than filtering what it
    // paints: no capture path then has to be trusted to leave the desktop out.
    // Requests made meanwhile are held and complete after the unlock, so a
    // screen share survives the lock instead of being torn down by it.
    void setCaptureAllowed(std::function<bool()> allowed);

    // Fails every frame currently in flight as retryable. A capture asked for
    // before the session blocked must not complete afterwards, whatever stage
    // it reached: refusing only new requests would let a frame already rendered
    // be handed over after the screen was supposed to be secure.
    void failPending();

    // Window sources come from ext_foreign_toplevel_handle_v1 objects the
    // toplevel list owns; it installs the lookup here.
    void setToplevelSourceResolver(ToplevelSourceResolver resolver);

    // DMA-BUF capture targets are advertised from this renderer's probe; null
    // keeps every session SHM-only.
    void setRenderer(const render::Renderer *renderer);

    wl_global *sourceGlobal() const;
    wl_global *toplevelSourceGlobal() const;
    wl_global *copyGlobal() const;
    bool hasPending(const QString &output) const;
    void renderPending(render::Renderer &renderer, const Painters &painters);
    void renderPending(render::Renderer &renderer, const std::function<void(bool)> &paint);

    // The composition was handed to the display, to be shown at `aimedNs`.
    // Frames already in their buffers are delivered now: waiting for the flip
    // behind a queued frame would capture every other refresh. GPU readbacks
    // into shared memory still wait for presentation.
    void drawn(uint64_t generation, uint64_t aimedNs);
    void presented(uint64_t generation, uint64_t monotonicNs);
    void discarded(uint64_t generation);

    // The pointer moved or changed image. A hardware cursor leaves the scene
    // untouched, so a session that paints cursors needs a composition to see it.
    void cursorChanged();

    struct State;

private:
    std::shared_ptr<State> m_state;
    wl_global *m_sourceGlobal = nullptr;
    wl_global *m_toplevelSourceGlobal = nullptr;
    wl_global *m_copyGlobal = nullptr;
};

} // namespace cutefish::wayland
