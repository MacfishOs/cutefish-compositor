// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "display/display_identity.h"
#include "display/native_display_delegate.h"
#include "render/color_transform.h"
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>
#include <vector>

struct wl_display;
struct wl_global;

namespace cutefish {
namespace backend {
class DrmBackend;
class DrmOutput;
} // namespace backend

namespace compositor {
class Compositor;
struct Output;
} // namespace compositor

namespace input {
class InputManager;
}
namespace render {
class Renderer;
class Texture;
} // namespace render

namespace wm {
class WindowManager;
}

namespace scene {
class Node;
class Scene;
class ScreenTransitionNode;
} // namespace scene

namespace cursor {
class Cursor;
class OutputCursorRouter;
} // namespace cursor

namespace display {
class DisplayManager;

class DisplayRuntime
{
public:
    DisplayRuntime(DisplayManager &manager, backend::DrmBackend *backend,
                   QList<compositor::Output *> models, std::vector<wl_global *> &globals,
                   wl_global *&primaryGlobal, wl_display *display,
                   compositor::Compositor &compositor, input::InputManager &input,
                   wm::WindowManager &wm, scene::Scene &scene, cursor::Cursor &cursor,
                   cursor::OutputCursorRouter *router, bool &updating);
    ~DisplayRuntime();
    DisplayRuntime(const DisplayRuntime &) = delete;
    DisplayRuntime &operator=(const DisplayRuntime &) = delete;
    static void initialize(DisplayManager &displayManager, backend::DrmBackend *backend,
                           const QList<compositor::Output *> &outputModels,
                           const QString &compositorInstanceId, compositor::Compositor &compositor,
                           input::InputManager &inputManager);
    std::optional<HardwareSnapshot> reprobe();
    bool secureScanout = false;
    void replaceOutputs(QList<compositor::Output *> models);
    ConfigurationResult configure(const DisplayConfiguration &configuration,
                                  NativeDisplayDelegate::ConfigureMode mode);
    ConfigurationResult setPower(const QList<DisplayId> &displays,
                                 NativeDisplayDelegate::PowerState state);

    bool isPoweredOff(const DisplayId &id) const
    {
        return powerStates.value(id, NativeDisplayDelegate::PowerState::On)
                != NativeDisplayDelegate::PowerState::On;
    }

    std::function<void()> lockRequested;
    std::function<bool(const QString &)> capturePending;
    // The largest scale of the applied displays, reported before the outputs
    // are re-sent. Xwayland renders at it, and sizes its screen from them.
    std::function<void(double)> maximumScaleChanged;
    // A frame reached this output and its KMS state holds nothing but the
    // compositor's own framebuffer. The session controller needs it per output:
    // a lock is not complete until every screen has shown the shield with no
    // application plane left beside it.
    std::function<void(const QString &)> securelyPresented;
    // Whether a frame drawn at the given frame time shows the lock transition over.
    std::function<bool(double)> secureContentSettled;

    // The session is blocked. Every output composites, and every commit clears
    // the application planes of the previous session frame. Applies to outputs
    // that appear later as well, so a hotplug or a DPMS wake mid-lock comes up
    // secure rather than inheriting the session's policy.
    void setSecureScanout(bool secure);
    void withdrawSecurePresentation();

    // The night light's tint, for every output and the ones that appear later.
    void setColorTransform(const render::ColorTransform &transform);

    bool isSecureScanout() const { return secureScanout; }

    // `layers` are what `transition` covers, painted in order.
    void setScreenTransition(scene::ScreenTransitionNode *transition, QList<scene::Node *> layers);
    // Each lit output's frame of what `paint` draws, keyed by output name, as
    // the desk switch animation starts from.
    std::map<QString, std::unique_ptr<render::Texture>>
    captureOutputs(const std::function<void(render::Renderer &)> &paint);

    // Called before the first layout is published: the frame each output shows
    // as the compositor starts, another DRM master's last one, stays in `cover`
    // over `layers` until `ready` holds, by default once a desktop covers every
    // output, then fades out. Upstream keeps its BootingAnimationController
    // widget on top for the same reason: no other UI blinks before the screen
    // it leads to is ready.
    void setStartupCover(scene::ScreenTransitionNode *cover, QList<scene::Node *> layers,
                         std::function<bool()> ready = {});

    // Whether the startup cover still hides the session, or is yet to. It
    // fades out a still copy, so nothing should move under it meanwhile.
    bool isStartupCoverShowing() const;

    // Blocked outputs carry over the frame they start on as well; for the login
    // screen, which has no session of its own on screen.
    bool carryOverWhileSecure = false;

    using NativeConfigure = std::function<ConfigurationResult(
            const DisplayConfiguration &, NativeDisplayDelegate::ConfigureMode)>;

    void setNativeConfigureForTesting(NativeConfigure configure)
    {
        nativeConfigure = std::move(configure);
    }

private:
    ConfigurationResult configureExact(const DisplayConfiguration &configuration,
                                       NativeDisplayDelegate::ConfigureMode mode);
    NativeConfigure nativeConfigure;
    void requireNativeRecovery();
    void publish();
    void captureOutgoingFrames();
    void startStartupCover();
    void bindFrames();
    static HardwareSnapshot discover(backend::DrmBackend *backend,
                                     const QList<compositor::Output *> &models,
                                     const QString &instance, uint64_t generation,
                                     DisplayIdentity &identity);
    QMetaObject::Connection activityConnection;
    QSet<DisplayId> wakeOnInput;
    DisplayIdentity identity;
    std::vector<std::unique_ptr<compositor::Output>> ownedModels;
    uint64_t nextFrameToken = 0;
    QSet<backend::DrmOutput *> boundOutputs;
    render::ColorTransform colorTransform;

    DisplayManager &displayManager;
    backend::DrmBackend *backend;
    QList<compositor::Output *> outputModels;
    std::vector<wl_global *> &outputGlobals;
    wl_global *&outputGlobal;
    wl_display *waylandDisplay;
    compositor::Compositor &compositor;
    input::InputManager &inputManager;
    wm::WindowManager &windowManager;
    scene::Scene &scene;
    cursor::Cursor &cursor;
    cursor::OutputCursorRouter *cursorRouter;
    bool &updatingOutput;
    QHash<DisplayId, NativeDisplayDelegate::PowerState> powerStates;
    scene::ScreenTransitionNode *screenTransition = nullptr;
    QList<scene::Node *> transitionLayers;
    QTimer transitionDeadline;
    scene::ScreenTransitionNode *startupCover = nullptr;
    QList<scene::Node *> startupCoverLayers;
    std::function<bool()> startupCoverReady;
    bool startupCoverPending = false;
    QTimer startupCoverDeadline;
};
} // namespace display
} // namespace cutefish
