// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/components/exo/display.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.
//
// Renamed from Display, which in Cutefish would be confused with a physical
// output, and reduced to the factories Cutefish needs.

#ifndef CUTEFISH_COMPOSITOR_COMPOSITOR_H_
#define CUTEFISH_COMPOSITOR_COMPOSITOR_H_

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QList>
#include <QObject>
#include <QRect>
#include <QString>

#include "compositor/output_scale.h"

namespace cutefish::render {
class DmaBufDevice;
class PresentationSubmission;
class Renderer;
struct DmaBufAttributes;
} // namespace cutefish::render

namespace cutefish::scene {
class Node;
class SurfaceNode;
} // namespace cutefish::scene

namespace cutefish::compositor {

class Buffer;
class ShmPool;
class Surface;

// Creates the compositor-side objects the Wayland protocol layer hands out, and
// owns the relationship between committed surface state and presented frames.
// Deliberately not a place for protocol handling or for window management.
class Compositor : public QObject
{
    Q_OBJECT

public:
    struct OutputView {
        QString id;
        QRect bounds;
        OutputScale scale;
        // The wl_output whose composition carries this view, which is what a
        // render context names; `id` is the logical display's session id.
        QString waylandName;

        friend bool operator==(const OutputView &, const OutputView &) = default;
    };

    // `surfaceContainer` is the scene node new surfaces are parented into. It
    // may be null in tests that only exercise protocol behavior.
    explicit Compositor(scene::Node *surfaceContainer, QObject *parent = nullptr);
    ~Compositor() override;

    // The wl_surface resource takes ownership of the returned Surface.
    std::unique_ptr<Surface> createSurface();

    // Takes ownership of `fd` in every case, including failure.
    std::shared_ptr<ShmPool> createShmPool(int fd, int32_t size);

    // The device linux-dmabuf buffers are imported through. Null when there is
    // no GPU renderer, in which case no linux-dmabuf global is advertised.
    render::DmaBufDevice *dmaBufDevice() const { return m_dmaBufDevice; }

    void setDmaBufDevice(render::DmaBufDevice *device) { m_dmaBufDevice = device; }

    // Takes ownership of the plane fds in `attributes` in every case, including
    // failure. Returns nullptr when the import fails.
    std::shared_ptr<Buffer> createDmaBufBuffer(render::DmaBufAttributes attributes);

    const std::vector<Surface *> &surfaces() const { return m_surfaces; }

    OutputScale outputScale() const { return m_outputScale; }

    void setOutputScale(OutputScale scale);
    void setOutputViews(QList<OutputView> outputs);

    const QList<OutputView> &outputViews() const { return m_outputViews; }

    void refreshOutputScales();
    uint32_t preferredScaleFor(const Surface *surface) const;

    // Surfaces that moved, resized, showed or hid since the last call.
    std::vector<std::shared_ptr<Surface *>> takeMovedSurfaces();

    // Whether a frame of the output showing `viewport` has frame callbacks or
    // presentation feedback to deliver, pixels aside.
    bool awaitsFrame(const QRect &viewport) const;

    // The scene node new surfaces are parented into. Null in tests that only
    // exercise protocol behavior.
    scene::Node *surfaceContainer() const { return m_surfaceContainer; }

    // Where input method candidate popups float, above every window. Hit
    // testing the session searches it before the surface container.
    void setInputMethodContainer(scene::Node *container) { m_inputMethodContainer = container; }
    scene::Node *inputMethodContainer() const { return m_inputMethodContainer; }

    // The surface owning `node`, or nullptr. Used by input hit testing, which
    // walks the scene in stacking order.
    Surface *surfaceForNode(const scene::Node *node) const;

    // Uploads every surface whose contents changed since the last frame.
    void updateTextures(render::Renderer &renderer);

    // Snapshot callbacks immediately before composition. Roll back on failed
    // submission or suspension; only the matching completion may finish them.
    void frameSubmitted(uint64_t generation = 0);
    void frameSubmittedForOutput(const QRect &viewport, uint64_t generation);
    // Discards the feedback an output's accepted attempt left out, for content
    // no other output can still show: hidden, wholly inside `viewport`, or on
    // no output. Content spanning outputs waits for one of them to present it,
    // or for its replacement.
    void abandonUnsubmittedPresentation(const QRect &viewport,
                                        const render::PresentationSubmission &submission);
    void frameDiscarded(uint64_t generation);
    // That frame was drawn and handed to the display: its callbacks are done.
    void frameDrawn(uint32_t timeMs, uint64_t generation = 0);

    // Called by Surface when committed state changed.
    void scheduleRepaint(bool decoration = false);

    // Surface lifecycle; not part of the public API.
    scene::SurfaceNode *createSurfaceNode(Surface *surface);
    void surfaceDestroyed(Surface *surface);

Q_SIGNALS:
    void repaintNeeded(bool decoration = false);
    void outputScaleChanged();
    void outputViewsChanged();
    void outputRendered(render::Renderer &renderer);
    // An output is about to render the frame timed at `frameTimeMs`, on the
    // same clock renderer.frameTime uses. Animations that move scene nodes
    // rather than paint them advance here, before the frame is drawn.
    void frameStarted(double frameTimeMs);
    // Handed to the display, which is expected to show it at `aimedNs`, on the
    // monotonic clock.
    void outputDrawn(uint64_t generation, uint64_t aimedNs);
    void outputPresented(uint64_t generation, uint64_t monotonicNs);
    void outputDiscarded(uint64_t generation);

private:
    OutputScale m_outputScale;
    QList<OutputView> m_outputViews;
    scene::Node *m_surfaceContainer = nullptr;
    scene::Node *m_inputMethodContainer = nullptr;
    render::DmaBufDevice *m_dmaBufDevice = nullptr;
    std::vector<Surface *> m_surfaces;
    // Hit testing asks this for every node it passes.
    std::unordered_map<const scene::Node *, Surface *> m_surfaceByNode;
    // Cascade counter, so surfaces the window manager never places do not
    // sit on top of each other.
    int m_placementIndex = 0;
};

} // namespace cutefish::compositor

#endif // CUTEFISH_COMPOSITOR_COMPOSITOR_H_
