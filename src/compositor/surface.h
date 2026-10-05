// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/surface.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: aura::Window/ui::Layer become a scene::SurfaceNode and
// the viz frame-submission half is replaced by the Cutefish renderer.

#ifndef CUTEFISH_COMPOSITOR_SURFACE_H_
#define CUTEFISH_COMPOSITOR_SURFACE_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QPainterPath>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QString>
#include <QTransform>

#include "compositor/buffer.h"
#include "render/glass.h"
#include "render/presentation.h"

namespace cutefish::render {

class DrmSyncobj;
class Renderer;
} // namespace cutefish::render

namespace cutefish::scene {

class Node;
class SurfaceNode;
} // namespace cutefish::scene

namespace cutefish::compositor {

class Compositor;
class SubSurface;
class SurfaceObserver;

// Counter-clockwise rotations, matching wl_output.transform.
enum class Transform {
    Normal,
    Rotate90,
    Rotate180,
    Rotate270,
    Flipped,
    FlippedRotate90,
    FlippedRotate180,
    FlippedRotate270,
};

// The point of a window that a client-initiated resize keeps in place.
enum class ResizeAnchor {
    TopLeft,
    Center,
};

// Client pacing notification, independent of measured presentation feedback.
// A missing time destroys the wl_callback silently.
using FrameCallback = std::function<void(std::optional<uint32_t> presentationTimeMs)>;

// The compositor-side view of a wl_surface.
//
// State is committed in three steps: requests update pending state, commit()
// transfers pending to cached state, and commitSurfaceHierarchy() applies cached
// state to current state. Synchronized subsurfaces wait for their parent here.
//
// A commit whose buffer the client's GPU is still writing is held, and with it
// every later commit in the same surface tree, until the buffer is ready.
//
// Some fields persist between commits (which buffer is attached, the regions,
// scale and transform) and some do not (damage, frame callbacks). Attachment
// is an optional so that an unrelated commit cannot silently drop the buffer:
// an unset optional means "attach was not called since the last commit".
class Surface
{
public:
    explicit Surface(Compositor *compositor);
    Surface(const Surface &) = delete;
    Surface &operator=(const Surface &) = delete;
    ~Surface();

    bool skipTaskbar() const { return m_skipTaskbar; }
    void setSkipTaskbar(bool skip);
    bool alwaysOnTop() const { return m_alwaysOnTop; }
    void setAlwaysOnTop(bool alwaysOnTop);
    // Read when the client resizes its window; no observer needs to know.
    ResizeAnchor resizeAnchor() const { return m_resizeAnchor; }
    void setResizeAnchor(ResizeAnchor anchor) { m_resizeAnchor = anchor; }
    // Read when the pointer or a touch reaches the window's edges.
    bool serverResize() const { return m_serverResize; }
    void setServerResize(bool enabled) { m_serverResize = enabled; }
    // Read when the window maps and unmaps; see set_restore_id.
    const QString &restoreId() const { return m_restoreId; }
    void setRestoreId(const QString &id) { m_restoreId = id; }

    struct MenuAddress {
        QString service;
        QString path;
        bool operator==(const MenuAddress &) const = default;
    };

    // Menu metadata is immediate and independent of buffer commits.
    uint64_t addMenuSource();
    void setMenuAddress(uint64_t source, const MenuAddress &address);
    void removeMenuSource(uint64_t source);

    const MenuAddress &menuAddress() const { return m_menuAddress; }

    // --- Requests. All of these are pending until commit(). ---

    void attach(const std::shared_ptr<Buffer> &buffer, const QPoint &offset);
    void damage(const QRect &rect);
    void damageBuffer(const QRect &rect);
    void requestFrameCallback(FrameCallback callback);
    void requestPresentationCallback(render::PresentationContent::Callback callback);
    void abandonUnsubmittedPresentation(const render::PresentationSubmission &submission);

    void setBackgroundBlur(const QRect &rect, int radius);

    QRect backgroundBlurRect() const { return m_state.basicState.blurRect; }

    int backgroundBlurRadius() const { return m_state.basicState.blurRadius; }

    // Surface-local; empty for the rounded rectangle.
    void setBackgroundBlurShape(const QPainterPath &shape)
    {
        m_pending.basicState.blurShape = shape;
    }

    // The committed shape clipped to the blur rectangle; empty when there is
    // none, no rectangle, or glass.
    QPainterPath backgroundBlurShape() const;

    void setGlass(const render::GlassParams &glass) { m_pending.basicState.glass = glass; }

    // The material a further glass request should build on, which is the pending
    // one: requests within a commit accumulate.
    const render::GlassParams &pendingGlass() const { return m_pending.basicState.glass; }

    const render::GlassParams &glass() const { return m_state.basicState.glass; }

    void setOpaqueRegion(const QRegion &region);
    void setInputRegion(const QRegion &region);
    void resetInputRegion();

    void setCrop(std::optional<QRectF> crop) { m_pending.basicState.crop = crop; }

    void setViewport(std::optional<QSize> viewport) { m_pending.basicState.viewport = viewport; }

    std::optional<QRectF> crop() const { return m_state.basicState.crop; }

    std::optional<QSize> viewport() const { return m_state.basicState.viewport; }

    uint32_t preferredScale() const;
    void notifyOutputScaleChanged();

    bool hasFractionalScale() const { return m_hasFractionalScale; }

    void setHasFractionalScale(bool value) { m_hasFractionalScale = value; }

    bool hasViewport() const { return m_hasViewport; }

    void setHasViewport(bool value) { m_hasViewport = value; }
    enum class ViewportError {
        None,
        BadSize,
        OutOfBuffer
    };
    ViewportError cachedViewportError() const;
    QTransform surfaceToBuffer() const;
    QTransform textureMapping() const;

    void setBufferScale(int scale);
    // Pending state, applied at the next commit. See State::clientScale.
    void setClientScale(double scale);
    void setBufferTransform(Transform transform);

    // Explicit synchronization, from wp_linux_drm_syncobj_surface_v1. All are
    // double-buffered and apply to the buffer attached in the same commit; the
    // protocol layer enforces that they come in pairs with a buffer.
    void setAcquireFence(render::SyncFence fence);
    void setAcquirePoint(std::shared_ptr<render::DrmSyncobj> timeline, uint64_t point);
    void setBufferRelease(std::shared_ptr<BufferRelease> release);

    bool hasPendingAcquireFence() const
    {
        return m_pending.acquireFence.isValid() || m_pending.acquireTimeline;
    }

    bool hasPendingBufferRelease() const { return m_pending.release != nullptr; }

    // Whether the most recent commit carried a non-null buffer attachment: the
    // one requested last, or inside onSurfaceCommit() the one being applied.
    // Explicit sync needs this and not hasContents(): a synchronized subsurface
    // commits its buffer into cached state without applying it yet.
    bool lastCommitAttachedBuffer() const { return m_lastCommitAttachedBuffer; }

    // Whether that buffer can be synchronized explicitly, through fences.
    bool lastCommitBufferSupportsExplicitSync() const
    {
        return m_lastCommitBufferSupportsExplicitSync;
    }

    // Caches pending state once its buffer is ready; applies it atomically if
    // not synchronized.
    void commit();

    // Commits waiting for their buffer, or behind one that is.
    size_t heldCommitCount() const { return m_heldCommits.size(); }

    bool isSynchronized() const;
    Surface *parentSurface() const;

    SubSurface *subSurface() const { return m_subSurface; }

    bool isMapped() const;
    void commitSurfaceHierarchy(bool synchronized);
    // Applies a desynchronized surface's cached state from the root down, as
    // Exo's SubSurface::OnSurfaceCommit hands the commit up to the root's
    // delegate, so every ancestor's hit test region takes the change in.
    void commitDesynchronized();

    // --- Roles and observers. ---

    // A surface may take at most one role. SubSurface sets this itself; other
    // role owners (the pointer cursor, later xdg-shell) call setHasRole().
    bool hasRole() const { return m_subSurface != nullptr || m_hasRole; }

    void setHasRole(bool hasRole) { m_hasRole = hasRole; }

    void addSurfaceObserver(SurfaceObserver *observer);
    void removeSurfaceObserver(SurfaceObserver *observer);
    bool hasSurfaceObserver(const SurfaceObserver *observer) const;

    // --- Input. ---

    // Subsurfaces in stacking order, bottom to top, with their positions
    // relative to this surface. The entry whose surface is `this` is the
    // sentinel marking where the parent's own content sits in the stack.
    const std::vector<std::pair<Surface *, QPoint>> &subSurfaceStack() const
    {
        return m_subSurfaceStack;
    }

    // This surface's own input region, clipped to its content. Does not include
    // subsurfaces.
    bool hitTest(const QPoint &point) const { return m_localHitTestRegion.contains(point); }

    QRegion localHitTestRegion() const { return m_localHitTestRegion; }

    // The input region of this surface and all its subsurfaces, in
    // surface-local coordinates. Matches Exo's Surface::hit_test_region_.
    QRegion hitTestRegion() const { return m_hitTestRegion; }

    // --- Pending state, for tests and diagnostics. ---

    bool hasPendingAttachedBuffer() const;

    QRegion pendingDamage() const { return m_pending.damage; }

    QRegion pendingBufferDamage() const { return m_pending.bufferDamage; }

    int pendingBufferScale() const { return m_pending.basicState.bufferScale; }

    Transform pendingBufferTransform() const { return m_pending.basicState.bufferTransform; }

    QRegion pendingOpaqueRegion() const { return m_pending.basicState.opaqueRegion; }

    std::optional<QRegion> pendingInputRegion() const { return m_pending.basicState.inputRegion; }

    size_t pendingFrameCallbackCount() const { return m_pending.frameCallbacks.size(); }

    // --- Committed state. ---

    std::shared_ptr<Buffer> buffer() const;
    QSize bufferSize() const;

    QPoint offset() const { return m_state.basicState.offset; }

    int bufferScale() const { return m_state.basicState.bufferScale; }

    double clientScale() const { return m_state.basicState.clientScale; }

    Transform bufferTransform() const { return m_state.basicState.bufferTransform; }

    QRegion opaqueRegion() const { return m_state.basicState.opaqueRegion; }

    std::optional<QRegion> inputRegion() const { return m_state.basicState.inputRegion; }

    // Committed damage in surface-local coordinates. Buffer damage is folded in
    // through the buffer scale and transform.
    QRegion damageRegion() const;

    // Buffer size mapped through the buffer scale and transform.
    QSize contentSize() const;

    bool hasContents() const { return !bufferSize().isEmpty(); }

    // Whether the committed content is opaque over its whole geometry, either
    // because the format has no alpha or because the client said so.
    bool isFullyOpaque() const;

    size_t committedFrameCallbackCount() const { return m_state.frameCallbacks.size(); }

    size_t inFlightFrameCallbackCount() const { return m_inFlightFrameCallbacks.size(); }

    // Whether the next output frame showing the surface has frame callbacks
    // to start or presentation feedback to settle for it.
    bool awaitsFrame() const;

    // Takes note of where the surface shows and whether it shows at all, and
    // says whether that changed since the last call: only then can it have
    // entered or left an output, or come to prefer another scale.
    bool updatePlacement();

    // --- Scene and rendering. ---

    void setExternalInputRegion(std::optional<QRegion> region);

    scene::SurfaceNode *node() const { return m_node; }

    QPoint position() const { return m_position; }

    // Hidden by the window manager (a minimized window). Independent of
    // mapping: the surface keeps its buffer, its state and its role, it is
    // simply not composited and not hit tested.
    bool isHidden() const { return m_hidden; }

    void setHidden(bool hidden);

    void setPosition(const QPoint &position);

    // Moves this surface's scene node under `newParent`, or detaches it from
    // the scene entirely when `newParent` is null. Used by the pointer to lift
    // a cursor surface out of the normal surface stack.
    void reparentNode(scene::Node *newParent);

    // Uploads the committed buffer into the node's texture if it changed since
    // the last call, and lets the buffer go. Returns true if anything was
    // uploaded.
    bool updateTexture(render::Renderer &renderer);

    // Snapshot applied callbacks for the scene about to be composed. Nonzero
    // generations include only mapped, effectively visible surfaces; generation
    // zero retains the legacy direct test API. No cached callbacks are moved.
    void frameSubmitted(uint64_t generation = 0);
    void frameDiscarded(uint64_t generation);

    std::shared_ptr<Surface *> lifetimeToken() const { return m_lifetime; }

    // Completes those callbacks.
    void frameDrawn(uint32_t timeMs, uint64_t generation = 0);

private:
    friend class SubSurface;
    struct CommitRequest;

    void addSubSurface(Surface *child);
    void removeSubSurface(Surface *child);
    // wl_subsurface.set_desync, in order with the tree's held commits.
    void requestDesynchronizedCommit();
    void applyCommit(CommitRequest &request);
    void holdCommit(std::unique_ptr<CommitRequest> request);
    // Applies the tree's held commits, oldest first, up to one still waiting.
    void applyReadyCommits();
    Surface *rootSurface();
    Surface *oldestHeldCommit();
    void setSubSurfacePosition(Surface *child, const QPoint &position);
    bool placeSubSurface(Surface *child, Surface *reference, bool above);
    void applyCachedState();
    void updateSubSurfaceStacking();
    void updateMapping();
    void updateHitTestRegion();
    void notifyWindowHintsChanged();

    struct SubSurfaceEntry {
        Surface *surface;
        QPoint position;
    };

    using SubSurfaceList = std::list<SubSurfaceEntry>;
    // The parent is a sentinel entry, permitting children on either side.
    SubSurfaceList m_pendingSubSurfaces;
    SubSurfaceList m_cachedSubSurfaces;
    SubSurfaceList m_subSurfaces;
    SubSurface *m_subSurface = nullptr; // non-owning role, owned by wl_resource
    bool m_hadSubSurfaceRole = false;
    bool m_hasRole = false;
    bool m_hasFractionalScale = false;
    bool m_hasViewport = false;
    std::unique_ptr<scene::Node> m_detachedNode;
    bool m_needsCommitSurface = false;

    struct State {
        std::optional<QRectF> crop;
        std::optional<QSize> viewport;
        QRect blurRect;
        int blurRadius = 0;
        QPainterPath blurShape;
        render::GlassParams glass;
        QRegion opaqueRegion;
        // Unset means "the whole surface accepts input", which is the default
        // and is not the same as an empty region.
        std::optional<QRegion> inputRegion;
        int bufferScale = 1;
        // How many of the client's surface units make one logical unit. Only
        // Xwayland has one: it draws at device resolution, and a buffer of
        // scale 1 maps onto size / clientScale logical units.
        double clientScale = 1;
        Transform bufferTransform = Transform::Normal;
        QPoint offset;
    };

    // Exo keeps a weak reference plus the size, so that a client destroying its
    // wl_buffer while it is committed cannot dangle and the surface still knows
    // how large its content is.
    struct BufferAttachment {
        std::weak_ptr<Buffer> buffer;
        QSize size;
    };

    struct ExtendedState {
        State basicState;
        // Persisted between commits; unset in the pending state means attach()
        // was not called since the last commit.
        std::optional<BufferAttachment> buffer;
        // Not persisted between commits.
        QRegion damage;
        QRegion bufferDamage;
        bool fullDamage = false;
        std::vector<FrameCallback> frameCallbacks;
        // Not persisted between commits, and never copied: they belong to
        // exactly one buffer attachment. The acquire point only lives until
        // commit, which waits for it.
        render::SyncFence acquireFence;
        std::shared_ptr<render::DrmSyncobj> acquireTimeline;
        uint64_t acquirePoint = 0;
        std::shared_ptr<BufferRelease> release;
    };

    QRect bufferRectToSurface(const QRect &rect) const;
    QRect surfaceRectToBuffer(const QRect &rect) const;

    Compositor *m_compositor = nullptr;
    // Owned by its parent in the scene tree; detached in the destructor.
    scene::SurfaceNode *m_node = nullptr;
    std::optional<QRegion> m_externalInputRegion;
    QPoint m_position;
    bool m_hidden = false;

    std::shared_ptr<render::PresentationContent> m_pendingPresentation;
    std::shared_ptr<render::PresentationContent> m_cachedPresentation;
    std::vector<std::weak_ptr<render::PresentationContent>> m_presentations;
    ExtendedState m_pending;
    ExtendedState m_cached;
    ExtendedState m_state;

    // The committed buffer contents have not reached a texture yet.
    std::shared_ptr<Buffer> m_uploadBuffer;
    bool m_textureDirty = false;
    // What the buffer changed since the texture was last uploaded, in buffer
    // pixels, unless a new mapping leaves only a whole upload.
    QRegion m_uploadDamage;
    bool m_uploadAll = true;
    bool m_lastCommitAttachedBuffer = false;
    bool m_lastCommitBufferSupportsExplicitSync = false;

    // Callbacks for state that has been drawn into the frame now on its way to
    // the display.
    std::vector<FrameCallback> m_inFlightFrameCallbacks;
    uint64_t m_frameGeneration = 0;

    struct Placement {
        QRect bounds;
        bool shown = false;

        bool operator==(const Placement &) const = default;
    };

    Placement m_placement;
    void updateMenuAddress();
    std::map<uint64_t, MenuAddress> m_menuSources;
    uint64_t m_nextMenuSource = 0;
    MenuAddress m_menuAddress;
    bool m_skipTaskbar = false;
    bool m_alwaysOnTop = false;
    ResizeAnchor m_resizeAnchor = ResizeAnchor::TopLeft;
    bool m_serverResize = false;
    QString m_restoreId;

    std::shared_ptr<Surface *> m_lifetime = std::make_shared<Surface *>(this);

    // Committed input geometry, refreshed by commitSurfaceHierarchy().
    QRegion m_localHitTestRegion;
    QRegion m_hitTestRegion;
    std::vector<std::pair<Surface *, QPoint>> m_subSurfaceStack;

    std::vector<SurfaceObserver *> m_observers;

    std::deque<std::unique_ptr<CommitRequest>> m_heldCommits;
};

} // namespace cutefish::compositor

#endif // CUTEFISH_COMPOSITOR_SURFACE_H_
