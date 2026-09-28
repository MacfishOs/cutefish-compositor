// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_BACKEND_DRM_DRM_OUTPUT_H_
#define CUTEFISH_BACKEND_DRM_DRM_OUTPUT_H_

#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <xf86drmMode.h>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QTimer>

#include "backend/drm/drm_commit_coordinator.h"
#include "backend/drm/drm_plane.h"
#include "backend/drm/drm_properties.h"
#include "backend/drm/presentation_tracker.h"
#include "backend/drm/scanout_controller.h"
#include "render/cursor_sink.h"
#include "render/output_render_context.h"
#include "render/output_scheduler.h"
#include "render/texture.h"
#include "scene/overlay_candidate.h"

struct gbm_bo;
struct gbm_surface;

namespace cutefish::render {

class EglRenderer;
class EglRenderTarget;
class Renderer;
} // namespace cutefish::render

namespace cutefish::backend {

class DrmBackend;
class DrmCursor;
class DrmDeviceApi;

// One connector driven by one CRTC, compositing onto the primary plane and,
// where the hardware allows it, scanning client buffers out of overlay planes
// and the cursor plane directly.
class DrmOutput : public QObject, public render::CursorSink
{
    Q_OBJECT

public:
    struct Config {
        uint32_t connectorId = 0;
        uint32_t crtcId = 0;
        // Index into drmModeRes, which is what possible_crtcs refers to.
        int crtcIndex = -1;
        uint32_t planeId = 0;
        drmModeModeInfo mode = {};
        QList<drmModeModeInfo> modes;
        QString name;
        QByteArray edid;
        QString portPath;
        QSize physicalSizeMm;
        int subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
        DrmPropertyMap connectorProperties;
        DrmPropertyMap crtcProperties;
        DrmPropertyMap planeProperties;
        // Every plane on the device, in ascending id.
        std::vector<DrmPlane> planes;
    };

    // Property names an atomic commit cannot do without.
    static QList<QByteArray> requiredConnectorProperties();
    static QList<QByteArray> requiredCrtcProperties();
    static QList<QByteArray> requiredPlaneProperties();

    DrmOutput(DrmBackend *backend, Config config, QObject *parent = nullptr);
    DrmOutput(DrmDeviceApi *device, Config config, std::function<bool()> sessionActive,
              bool monotonicClock, QObject *parent = nullptr,
              std::function<render::OutputScheduler::Time()> now =
                      render::OutputScheduler::Clock::now);
    ~DrmOutput() override;

    struct CompositedBuffer {
        CompositedBuffer() = default;
        CompositedBuffer(const CompositedBuffer &) = delete;
        CompositedBuffer &operator=(const CompositedBuffer &) = delete;
        PlaneContent primary;
        std::function<void()> release;

        ~CompositedBuffer()
        {
            if (release)
                release();
        }
    };

    using AcquireBuffer = std::function<std::unique_ptr<CompositedBuffer>()>;
    using CreateTarget = std::function<std::unique_ptr<render::EglRenderTarget>(QSize)>;
    bool init(render::EglRenderer *renderer, std::unique_ptr<render::EglRenderTarget> target,
              AcquireBuffer acquire, CreateTarget createTarget = {});

    uint32_t connectorId() const { return m_config.connectorId; }

    uint32_t crtcId() const { return m_config.crtcId; }

    // Where this CRTC's cursor plane showed an image when the device was probed,
    // in CRTC pixels: at startup, the cursor the previous DRM master left.
    std::optional<QPoint> probedCursor() const;

    QByteArray edid() const { return m_config.edid; }

    QString portPath() const
    {
        return m_config.portPath.isEmpty() ? m_config.name : m_config.portPath;
    }

    // `keepRoute`: the output replacing this one drives the same connector from
    // the same CRTC, so the route stays lit on its last frame until the new
    // output's first commit replaces it, unless that frame shows a client
    // buffer, which is only released once KMS has stopped reading it.
    bool retireForReprobe(bool connectorExists, bool keepRoute = false);

    QString name() const { return m_config.name; }

    QString publicName() const;

    DrmBackend *backend() const { return m_backend; }

    QSize size() const;

    QSize physicalSizeMm() const { return m_config.physicalSizeMm; }

    int subpixel() const { return m_config.subpixel; }

    double refreshRate() const;

    // The rate frames are scheduled and reported at: the mode's refresh, or
    // 60 Hz once flips show the device has no vblank to back that refresh.
    double pacingRate() const;

    const drmModeModeInfo &mode() const { return m_config.mode; }

    const QList<drmModeModeInfo> &modes() const { return m_config.modes; }

    // Creates the GBM surface and the renderer's target.
    bool init(render::EglRenderer *renderer);

    // Draws the first frame and performs the initial atomic modeset.
    bool enable();

    // Stops wakeups. Retains potentially scanned-out BOs until KMS retires them.
    void disable();

    bool isEnabled() const { return m_enabled; }

    bool isBlanked() const { return m_blanked; }

    bool requiresSoftwareBlanking() const;

    void setLinearBuffers(bool linear) { m_linearBuffers = linear; }
    // The GBM surface frames for this output are rendered into.
    gbm_surface *createSurface(const QSize &size) const;

    struct Configuration {
        DrmOutput *output;
        drmModeModeInfo mode;
        bool enabled;
        bool blanked = false;
    };

    static bool configure(const std::vector<Configuration> &configuration, bool testOnly,
                          DrmCommitStatus *status = nullptr, QString *diagnostic = nullptr,
                          DrmDeviceApi **failedDevice = nullptr);

    // Painting is a callback so DRM/KMS stays independent of the scene.
    void setRenderCallback(std::function<void(render::Renderer &, quint64)> callback);

    // Per-frame work that has to happen whether or not the scene is composited:
    // texture uploads and frame-callback accounting. Runs with the GL context
    // current, before the scene is painted.
    void setPrepareCallback(std::function<void(render::Renderer &, quint64)> callback);

    // Called as each scheduled frame begins, with its frame time in fractional
    // milliseconds, so what steps on frames steps whether or not this output
    // draws. Returns whether the output has anything to draw; a frame with
    // nothing is skipped rather than composited and flipped unchanged.
    void setBeginFrameCallback(std::function<bool(double)> callback);

    // Asked once per frame for the content that could go on hardware planes.
    // Absent, or returning an empty plan, means everything is composited.
    void setScanoutPlanner(std::function<scene::OverlayPlan(const QSize &, size_t)> planner);

    // Paints `context.logicalViewport` into a texture outside the frame cycle.
    // Nothing is presented; the next frame repaints in full.
    std::unique_ptr<render::Texture>
    renderOffscreen(const render::OutputRenderContext &context,
                    const std::function<void(render::Renderer &)> &paint);

    // Client buffers may be scanned out directly instead of composited.
    // Enabled by default; tests and troubleshooting turn it off.
    void setDirectScanoutEnabled(bool enabled);

    // While set, a modeset that carries over the CRTC's current frame also keeps
    // a copy of it, for the scene to hold on screen until the session is ready.
    void setKeepCarriedFrame(bool keep) { m_keepCarriedFrame = keep; }

    // The copy the last such modeset kept, if any; the caller owns it.
    std::unique_ptr<render::Texture> takeCarriedFrame() { return std::move(m_carriedFrame); }

    // The login screen is blocked from its first frame but has no session of its
    // own on screen to protect, so it carries over the frame it starts on too.
    void setCarryOverWhileSecure(bool carry) { m_carryOverWhileSecure = carry; }

    // The session is blocked. The scanout planner is not consulted, and every
    // commit explicitly turns off every plane but the composited primary, so no
    // application buffer from the previous session frame can still be in the
    // KMS state. Set before the transition's first frame, not after it.
    void setSecureScanout(bool secure);

    bool isSecureScanout() const { return m_secureScanout; }

    // A session transition began: frames drawn before it, on screen or still on
    // their way, show where it starts and must not complete it.
    void withdrawSecurePresentation();

    // Whether a frame drawn at the given frame time shows the session's lock
    // transition over; one drawn mid-fade is not secure, whatever its planes.
    void setSecureContentCheck(std::function<bool(double)> check)
    {
        m_secureContentCheck = std::move(check);
    }

    // Whether the frame now on screen was committed in secure mode with nothing
    // but the compositor's own framebuffer programmed. The session's
    // presentation barrier waits for this, so "locked" cannot be reported while
    // a previous-session plane is still live. A dark output is secure by
    // definition and never holds the barrier open.
    bool lastPresentedFrameWasSecure() const;

    // Whether `planes` is the compositor's own framebuffer and nothing else.
    // The definition of "no application plane is live", in one place.
    static bool isSecureConfiguration(const std::vector<PlaneContent> &planes);

    bool isDirectScanoutEnabled() const { return m_directScanoutEnabled; }

    // render::CursorSink: the KMS cursor plane, when the device has one.
    bool setCursorImage(const render::CursorImage &image) override;
    void hideCursor() override;
    void moveCursor(const QPointF &position) override;

    bool hasHardwareCursor() const { return m_cursor != nullptr; }

    // Frames whose primary plane showed a client buffer rather than the
    // composited scene.
    quint64 directScanoutFrames() const { return m_directScanoutFrames; }

    quint64 overlayFrames() const { return m_overlayFrames; }

    using Reason = render::OutputScheduler::Reason;
    void scheduleFrame(Reason reason = Reason::Scene);

    struct FlipEvent {
        QPointer<DrmOutput> output;
        uint64_t frame;
        bool delivered = false;
    };

    void setEventRetirementHandler(std::function<void(std::unique_ptr<FlipEvent>)> handler)
    {
        m_retireEvent = std::move(handler);
    }

    // How long flips may go without a kernel event before the output stops
    // waiting for the oldest one.
    void setFlipTimeout(std::chrono::milliseconds timeout) { m_flipTimeout.setInterval(timeout); }

    static void dispatchPageFlip(FlipEvent event, uint32_t seconds, uint32_t microseconds,
                                 uint32_t sequence);
    void onPageFlip(uint64_t frame, uint32_t seconds, uint32_t microseconds, uint32_t sequence);
    std::shared_ptr<render::PresentationOutput> presentationOutput;
    std::function<void(const render::PresentationSubmission &)> presentationSubmitted;
    void reportStats() const;
    // The same counters as reportStats() logs, for a debug interface to read.
    QString statsLine() const;

    // Output renders (including initial modesets), not completion events.
    quint64 frameCount() const { return m_frameCount; }

Q_SIGNALS:
    // The frame was composed and handed to KMS, committed or queued behind a
    // pending flip: viz's DrawAndSwap, where it acks the frames it drew.
    // `aimedNs` is the refresh it was drawn for, on the monotonic clock.
    void frameDrawn(quint64 generation, quint64 aimedNs);
    // Matched normal submission completed. Scheduling uses a separate
    // monotonic nominal clock.
    void framePresented(quint32 timeMs, quint64 generation);
    void imagePresented(quint64 generation, quint64 monotonicNs);
    void frameDiscarded(quint64 generation);

private:
    // The planes and the GBM buffer one committed frame is made of.
    struct SubmittedFrame {
        render::PresentationSubmission presentation;
        std::vector<PlaneContent> planes;
        // The composited buffer on the primary plane, or null when the frame
        // scanned a client buffer out directly.
        std::unique_ptr<CompositedBuffer> composited;
        // The CRTC's OUT_FENCE for this commit: it signals when the commit
        // reaches the screen, which is when the frame it replaces stops being
        // read. That makes it the release fence for the previous frame.
        render::SyncFence releaseFence;
        // Signalled when the GPU finished composing the primary plane's buffer.
        render::SyncFence drawnFence;
        // Committed in secure mode with no application plane programmed.
        bool secure = false;
    };

    // A frame rendered in its own refresh slot while the previous one was
    // still on its way to the screen. KMS holds one flip per CRTC, so it is
    // committed the moment that flip lands, the way ui::GbmSurfaceless keeps
    // unsubmitted frames and schedules the next page flip from OnPresentation.
    struct QueuedFrame {
        uint64_t frame = 0;
        SubmittedFrame submitted;
    };

    std::unique_ptr<CompositedBuffer> acquireComposited(gbm_surface *surface);
    void commitQueued();
    // Forgets every frame rendered but not presented, returning their tokens
    // for frameDiscarded once the caller's state is consistent.
    QList<quint64> dropOutstandingFrames();
    bool renderFrame(uint64_t frame, bool modeset);
    // One attempt at a frame. `allowPromotion` false forces plain composition,
    // which is the fallback when a promoted configuration fails to commit.
    bool renderAttempt(uint64_t frame, bool modeset, bool allowPromotion, bool &prepared,
                       bool retry = false);
    void wakeup();
    void armWakeup();
    // The refresh the frame begun last is drawn for, in monotonic nanoseconds.
    quint64 aimedNs() const;
    // What follows a flip, landed or given up on: the pending buffers go on
    // screen, the queued frame is committed and the scheduler moves on.
    void finishFlip(uint64_t frame, uint32_t timeMs,
                    std::optional<render::OutputScheduler::Time> presented);
    void onFlipTimeout();
    // The kernel keeps a committed event's address as its cookie, so it must
    // outlive this output.
    void retireEvent(std::unique_ptr<FlipEvent> event);
    bool commit(const std::vector<PlaneContent> &planes, bool modeset, uint64_t frame = 0);
    // Upstream: ScreenManager::GetModesetPlanes and FillModesetBuffer, which
    // copy the CRTC's current framebuffer into the modeset buffer when the
    // sizes match and clear it black otherwise.
    bool fillModesetBuffer(render::EglRenderTarget *target, const QSize &size, bool copyScanout);
    bool createModeBlob();
    void destroyModeBlob();
    void releaseScanoutBuffers();
    void clearPromotions();
    // Hands the buffers of a retired frame back, with the fence that says when
    // the display actually stopped reading them.
    void retire(SubmittedFrame &frame, const render::SyncFence &fence);

    DrmBackend *m_backend = nullptr;
    Config m_config;
    DrmDeviceApi *m_device = nullptr;
    std::function<bool()> m_sessionActive;
    bool m_monotonicClock = false;
    AcquireBuffer m_acquireBuffer;
    CreateTarget m_createTarget;

    render::EglRenderer *m_renderer = nullptr;
    gbm_surface *m_gbmSurface = nullptr;
    std::unique_ptr<render::EglRenderTarget> m_target;

    uint32_t m_modeBlobId = 0;

    // Scanning out now; released only once KMS has moved on.
    gbm_bo *m_scanoutBuffer = nullptr;
    // Committed but not yet flipped to.
    gbm_bo *m_pendingBuffer = nullptr;

    std::function<void(render::Renderer &, quint64)> m_renderCallback;
    std::function<void(render::Renderer &, quint64)> m_prepareCallback;
    std::function<bool(double)> m_beginFrameCallback;
    // The next frame draws whatever the scene says: after a resume, a modeset
    // or a change in how frames are committed, the screen needs a new one.
    bool m_forceFrame = true;
    std::function<scene::OverlayPlan(const QSize &, size_t)> m_scanoutPlanner;

    std::unique_ptr<ScanoutController> m_scanout;
    std::unique_ptr<DrmCursor> m_cursor;
    // Nodes this frame took off the compositor's hands; cleared once the frame
    // has been committed, so a later fallback composites them again.
    std::vector<scene::SurfaceNode *> m_promoted;
    PresentationTracker m_presentations;
    SubmittedFrame m_pendingFrame;
    SubmittedFrame m_scanoutFrame;
    std::optional<QueuedFrame> m_queuedFrame;
    // The out-fence of the commit just issued, moved into the frame it belongs
    // to once that frame's bookkeeping is built.
    render::SyncFence m_lastReleaseFence;
    bool m_directScanoutEnabled = true;
    bool m_secureScanout = false;
    std::function<bool(double)> m_secureContentCheck;
    bool m_keepCarriedFrame = false;
    bool m_carryOverWhileSecure = false;
    std::unique_ptr<render::Texture> m_carriedFrame;
    bool m_lastPresentedSecure = false;
    quint64 m_directScanoutFrames = 0, m_overlayFrames = 0;

    bool m_enabled = false;
    bool m_blanked = false;
    bool m_linearBuffers = false;
    bool m_transferUnavailable = false;
    int m_transferRoute = -1;
    bool m_retiring = false;
    bool m_configuring = false;
    std::function<render::OutputScheduler::Time()> m_now;
    render::OutputScheduler m_scheduler;
    QTimer m_wakeup;
    render::OutputScheduler::Time m_armedDeadline {};
    QTimer m_flipTimeout;
    std::map<uint64_t, std::unique_ptr<FlipEvent>> m_events;
    std::function<void(std::unique_ptr<FlipEvent>)> m_retireEvent;
    uint64_t m_bufferFrame = 0;
    // Frames whose wl_surface.frame callbacks are still owed, oldest first:
    // committed, queued, or a modeset's; gone once presented or discarded.
    QList<quint64> m_outstanding;
    quint64 m_frameCount = 0, m_commits = 0, m_flipCommits = 0, m_completions = 0;
    std::clock_t m_cpuStart = std::clock();
    uint32_t m_lastSequence = 0, m_lastTimeMs = 0;
    bool m_unsynced = false;
    std::chrono::steady_clock::time_point m_statsStart = std::chrono::steady_clock::now();
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_DRM_DRM_OUTPUT_H_
