// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "display/display_runtime.h"
#include "backend/drm/drm_backend.h"
#include "backend/drm/drm_mode_fallback_planner.h"
#include "backend/drm/drm_output.h"
#include "backend/drm/drm_selection.h"
#include "compositor/compositor.h"
#include "compositor/output.h"
#include "compositor/surface.h"
#include "core/logging.h"
#include "cursor/cursor.h"
#include "cursor/output_cursor_router.h"
#include "display/display_configuration_validator.h"
#include "display/display_manager.h"
#include "display/display_preferences.h"
#include "display/edid.h"
#include "display/mirror_transform.h"
#include "input/input_manager.h"
#include "render/egl_renderer.h"
#include "render/renderer.h"
#include "scene/scene.h"
#include "scene/screen_transition_node.h"
#include "wayland/protocols/wl_output.h"
#include "wayland/server_util.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include <QSet>
#include <cmath>

namespace cutefish::display {

namespace {

// How long the outgoing frame may wait for the shell to redraw. Exo holds its
// orientation compositor lock for up to 2.5 s; the shell redraws far faster.
constexpr int kScreenTransitionTimeoutMs = 1000;

// How long the startup cover waits for the shell's desktop. Upstream's booting
// animation waits at most 5 s for the device before it plays anyway.
constexpr int kStartupCoverTimeoutMs = 5000;

} // namespace

DisplayRuntime::DisplayRuntime(DisplayManager &manager, backend::DrmBackend *nativeBackend,
                               QList<compositor::Output *> models,
                               std::vector<wl_global *> &globals, wl_global *&primaryGlobal,
                               wl_display *display, compositor::Compositor &compositor,
                               input::InputManager &input, wm::WindowManager &wm,
                               scene::Scene &scene, cursor::Cursor &cursor,
                               cursor::OutputCursorRouter *router, bool &updating)
    : displayManager(manager)
    , backend(nativeBackend)
    , outputModels(std::move(models))
    , outputGlobals(globals)
    , outputGlobal(primaryGlobal)
    , waylandDisplay(display)
    , compositor(compositor)
    , inputManager(input)
    , windowManager(wm)
    , scene(scene)
    , cursor(cursor)
    , cursorRouter(router)
    , updatingOutput(updating)
{
    activityConnection = QObject::connect(
            &inputManager, &input::InputManager::userActivity, &displayManager, [this] {
                if (wakeOnInput.isEmpty())
                    return;
                const auto result =
                        setPower(wakeOnInput.values(), NativeDisplayDelegate::PowerState::On);
                if (!result.succeeded)
                    qWarning() << "Unable to wake displays:" << result.error.message;
            });
    identity.seed(displayManager.hardware()->displays);
    transitionDeadline.setSingleShot(true);
    // A coarse timer may fire before the node's own deadline, and then nothing
    // asks for the frame that ends the hold.
    transitionDeadline.setTimerType(Qt::PreciseTimer);
    transitionDeadline.callOnTimeout([this] {
        if (screenTransition)
            screenTransition->notifyChanged();
    });
    startupCoverDeadline.setSingleShot(true);
    startupCoverDeadline.setTimerType(Qt::PreciseTimer);
    startupCoverDeadline.callOnTimeout([this] {
        if (startupCover)
            startupCover->notifyChanged();
    });
    bindFrames();
    displayManager.setPublisher([this] {
        publish();
    });
    inputManager.setDisplayManager(&displayManager);
    publish();
}

DisplayRuntime::~DisplayRuntime()
{
    QObject::disconnect(activityConnection);
    if (backend) {
        const auto detach = [this](const auto &outputs) {
            for (const auto &output : outputs) {
                QObject::disconnect(output, nullptr, &displayManager, nullptr);
                output->presentationSubmitted = {};
                output->setPrepareCallback({});
                output->setRenderCallback({});
                output->setScanoutPlanner({});
            }
        };
        detach(backend->allOutputs());
        detach(backend->allOutputs(true));
    }
    cursor.setSink(nullptr);
    displayManager.setPublisher({});
    inputManager.setDisplayManager(nullptr);
    // The readiness check refers to this runtime.
    if (screenTransition)
        screenTransition->releaseRenderResources();
}

HardwareSnapshot DisplayRuntime::discover(backend::DrmBackend *backend,
                                          const QList<compositor::Output *> &models,
                                          const QString &instance, uint64_t generation,
                                          DisplayIdentity &identity)
{
    QList<cutefish::display::DisplaySnapshot> physicalDisplays;
    for (int index = 0; index < models.size(); ++index) {
        const auto *model = models.at(index);
        auto *nativeBackend = backend && index < backend->allOutputs().size()
                ? backend->allOutputs()[index]->backend()
                : nullptr;
        const auto displayMode = [](const drmModeModeInfo &nativeMode) {
            cutefish::display::DisplayMode mode;
            mode.pixelSize = QSize(nativeMode.hdisplay, nativeMode.vdisplay);
            mode.refreshMilliHz = static_cast<int>(
                    std::lround(cutefish::backend::refreshRate(nativeMode) * 1000.0));
            mode.pixelClockKHz = nativeMode.clock;
            mode.horizontalTotal = nativeMode.htotal;
            mode.verticalTotal = nativeMode.vtotal;
            mode.flags = nativeMode.flags;
            mode.name = QString::fromLatin1(nativeMode.name);
            mode.preferred = nativeMode.type & DRM_MODE_TYPE_PREFERRED;
            return mode;
        };
        cutefish::display::DisplayMode mode;
        QList<cutefish::display::DisplayMode> modes;
        if (backend && index < int(backend->allOutputs().size())) {
            const auto *drmOutput = backend->allOutputs()[index];
            mode = displayMode(drmOutput->mode());
            for (const auto &nativeMode : drmOutput->modes())
                modes.append(displayMode(nativeMode));
        } else {
            mode.pixelSize = model->pixelSize;
            mode.refreshMilliHz = model->refreshMilliHz;
            mode.pixelClockKHz = model->pixelSize.width() * model->pixelSize.height() * 60 / 1000;
            mode.horizontalTotal = model->pixelSize.width();
            mode.verticalTotal = model->pixelSize.height();
            mode.name = QStringLiteral("%1x%2@%3")
                                .arg(mode.pixelSize.width())
                                .arg(mode.pixelSize.height())
                                .arg(mode.refreshMilliHz);
            mode.preferred = model->preferred;
            modes = {mode};
            for (const QSize size : {QSize(1600, 900), QSize(1280, 720)}) {
                auto alternative = mode;
                alternative.pixelSize = size;
                alternative.pixelClockKHz = size.width() * size.height() * 60 / 1000;
                alternative.horizontalTotal = size.width();
                alternative.verticalTotal = size.height();
                alternative.name =
                        QStringLiteral("%1x%2@60000").arg(size.width()).arg(size.height());
                alternative.preferred = false;
                if (alternative.stableKey() != mode.stableKey())
                    modes.append(alternative);
            }
        }
        cutefish::display::DisplaySnapshot physical;
        EdidInfo edid;
        physical.persistentKey = QStringLiteral("session:%1").arg(model->model);
        physical.portKey = (nativeBackend ? nativeBackend->deviceKey() : QStringLiteral("virtual"))
                + QLatin1Char('/') + model->name;
        if (backend && index < int(backend->allOutputs().size())) {
            physical.portKey = nativeBackend->deviceKey() + QLatin1Char('/')
                    + backend->allOutputs()[index]->portPath();
            edid = parseEdid(backend->allOutputs()[index]->edid());
            physical.edidKey = edid.key;
        }
        const QString route = backend && index < int(backend->allOutputs().size())
                ? backend->allOutputs()[index]->portPath()
                : model->name;
        const QStringList connectorPath = route.split(QLatin1Char('-'));
        physical.baseConnectorKey =
                connectorPath.first().compare(QStringLiteral("DP"), Qt::CaseInsensitive) == 0
                        && connectorPath.size() > 2
                ? connectorPath.mid(0, 2).join(QLatin1Char('-'))
                : route;
        physical.pathTopology = connectorPath;
        physical.connectorName = model->name;
        physical.devicePath =
                nativeBackend ? nativeBackend->devicePath() : QStringLiteral("virtual");
        physical.make = edid.make.isEmpty() ? model->make : edid.make;
        physical.model = edid.model.isEmpty() ? model->model : edid.model;
        physical.physicalSizeMm = model->physicalSizeMm;
        const auto connector = nativeBackend ? backend->allOutputs()[index]->name() : model->name;
        physical.connectionType =
                (!nativeBackend || nativeBackend->driverName() == QStringLiteral("virtio_gpu")
                 || nativeBackend->driverName() == QStringLiteral("vmwgfx")
                 || nativeBackend->driverName() == QStringLiteral("qxl")
                 || nativeBackend->driverName() == QStringLiteral("vkms"))
                ? cutefish::display::DisplayConnectionType::Virtual
                : (connector.startsWith(QStringLiteral("eDP"), Qt::CaseInsensitive)
                   || connector.startsWith(QStringLiteral("LVDS"), Qt::CaseInsensitive)
                   || connector.startsWith(QStringLiteral("DSI"), Qt::CaseInsensitive))
                ? cutefish::display::DisplayConnectionType::Internal
                : cutefish::display::DisplayConnectionType::External;
        physical.capabilities = cutefish::display::DisplayCapability::Scale
                | cutefish::display::DisplayCapability::Rotation
                | cutefish::display::DisplayCapability::SoftwareMirror
                | cutefish::display::DisplayCapability::Power;
        physical.modes = modes;
        physical.currentMode = mode;
        physical.nativeMode = mode;
        for (const auto &candidate : modes) {
            if (candidate.preferred) {
                physical.nativeMode = candidate;
                break;
            }
        }
        physicalDisplays.append(std::move(physical));
    }
    identity.assign(physicalDisplays);
    for (auto *model : models) {
        for (const auto &physical : physicalDisplays) {
            if (physical.connectorName == model->name)
                model->displayId = physical.sessionId.value();
        }
    }
    return {instance, QStringLiteral("seat0"), generation, physicalDisplays};
}

void DisplayRuntime::initialize(DisplayManager &displayManager, backend::DrmBackend *backend,
                                const QList<compositor::Output *> &outputModels,
                                const QString &compositorInstanceId,
                                compositor::Compositor &compositor,
                                input::InputManager &inputManager)
{
    const auto &models = outputModels;
    DisplayIdentity identity;
    identity.rememberPersistentKeys(DisplayPreferences().persistentKeys());
    const auto snapshot = discover(backend, models, compositorInstanceId, 1, identity);
    if (!displayManager.installHardware(snapshot))
        return;

    QList<cutefish::display::LogicalDisplay> logicalDisplays;
    QList<cutefish::compositor::Compositor::OutputView> views;
    QList<QRect> topology;
    for (int index = 0; index < models.size(); ++index) {
        const auto *model = models.at(index);
        const cutefish::display::DisplayId id(models.at(index)->displayId);
        cutefish::display::LogicalDisplay logical;
        logical.id = id;
        logical.destinations = {id};
        logical.waylandName = model->name;
        logical.bounds = QRect(model->position, model->logicalSize());
        logical.workArea = logical.bounds;
        logical.scale = model->scale;
        logical.primary = index == 0;
        logicalDisplays.append(logical);
        views.append({id.value(), logical.bounds, logical.scale, model->name});
        topology.append(logical.bounds);
    }
    cutefish::display::AppliedConfiguration applied;
    applied.compositorInstanceId = compositorInstanceId;
    applied.topologyGeneration = 1;
    applied.revision = displayManager.applied() ? displayManager.applied()->revision + 1 : 1;
    applied.physicalDisplays = displayManager.hardware()->displays;
    applied.logicalDisplays = std::move(logicalDisplays);
    for (int index = 1; index < applied.logicalDisplays.size(); ++index)
        applied.placements.append({applied.logicalDisplays[index].id,
                                   applied.logicalDisplays[index - 1].id,
                                   cutefish::display::LayoutEdge::Right, 0});
    displayManager.installApplied(std::move(applied));
    compositor.setOutputViews(std::move(views));
    inputManager.setTopology(topology, topology.isEmpty() ? QRect() : topology.first());
}

void DisplayRuntime::replaceOutputs(QList<compositor::Output *> models)
{
    cursor.setSink(nullptr);
    if (cursorRouter)
        cursorRouter->setTargets({});
    // A connector that is still there keeps its wl_output global, with the
    // bindings and capture sources clients hold on it; publish() sends it the
    // new metrics. Only a connector that went away is withdrawn. Withdrawing
    // and re-announcing an unchanged output on every reprobe - the seat
    // activation at startup included - is a hotplug clients need not see, and
    // one SDL2 cannot survive while it recreates its window.
    std::vector<wl_global *> kept(models.size(), nullptr);
    for (int index = 0; index < outputModels.size() && index < int(outputGlobals.size());
         ++index) {
        auto *global = outputGlobals[index];
        if (!global)
            continue;
        int next = -1;
        for (int candidate = 0; candidate < models.size() && next < 0; ++candidate) {
            if (models.at(candidate)->name == outputModels.at(index)->name && !kept[candidate])
                next = candidate;
        }
        if (next < 0)
            wayland::removeOutputGlobal(global);
        else
            kept[next] = global;
    }
    outputGlobal = nullptr;
    outputGlobals = std::move(kept);
    outputModels = std::move(models);
    bindFrames();
}

std::optional<HardwareSnapshot> DisplayRuntime::reprobe()
{
    if (!backend)
        return std::nullopt;
    cursor.setSink(nullptr);
    if (cursorRouter)
        cursorRouter->setTargets({});
    if (!backend->reprobe())
        return std::nullopt;
    std::vector<std::unique_ptr<compositor::Output>> replacement;
    QList<compositor::Output *> models;
    for (const auto &output : backend->allOutputs()) {
        auto model = std::make_unique<compositor::Output>();
        model->name = output->publicName();
        model->model = output->name();
        model->pixelSize = output->size();
        model->physicalSizeMm = output->physicalSizeMm();
        models.append(model.get());
        replacement.push_back(std::move(model));
    }
    const auto hardware = discover(backend, models, displayManager.hardware()->compositorInstanceId,
                                   displayManager.hardware()->topologyGeneration + 1, identity);
    replaceOutputs(models);
    ownedModels = std::move(replacement);
    // A display that went away takes its power state and wake-on-input entry
    // along, or every later input event would try to power it on.
    QSet<DisplayId> present;
    for (const auto &display : hardware.displays)
        present.insert(display.sessionId);
    for (auto it = powerStates.begin(); it != powerStates.end();)
        it = present.contains(it.key()) ? std::next(it) : powerStates.erase(it);
    wakeOnInput.intersect(present);
    return hardware;
}

void DisplayRuntime::setSecureScanout(bool secure)
{
    if (secureScanout == secure)
        return;
    secureScanout = secure;
    if (!backend)
        return;
    for (const auto &output : backend->allOutputs())
        output->setSecureScanout(secure);
}

void DisplayRuntime::withdrawSecurePresentation()
{
    if (!backend)
        return;
    for (const auto &output : backend->allOutputs())
        output->withdrawSecurePresentation();
}

void DisplayRuntime::setScreenTransition(scene::ScreenTransitionNode *transition,
                                         QList<scene::Node *> layers)
{
    screenTransition = transition;
    transitionLayers = std::move(layers);
}

std::map<QString, std::unique_ptr<render::Texture>>
DisplayRuntime::captureOutputs(const std::function<void(render::Renderer &)> &paint)
{
    std::map<QString, std::unique_ptr<render::Texture>> frames;
    if (!backend)
        return frames;
    const auto outputs = backend->allOutputs();
    for (int index = 0; index < outputModels.size() && index < int(outputs.size()); ++index) {
        auto *model = outputModels.at(index);
        if (model->logicalExtent.isEmpty())
            continue;
        auto texture = outputs[index]->renderOffscreen(
                {model->name, QRect(model->position, model->logicalSize()), model->pixelSize,
                 model->renderScale, static_cast<uint32_t>(model->transform), 0,
                 model->nativeContentRect},
                paint);
        if (texture)
            frames[model->name] = std::move(texture);
    }
    return frames;
}

void DisplayRuntime::captureOutgoingFrames()
{
    if (!screenTransition || !backend)
        return;
    screenTransition->stopAnimating();
    const auto &applied = *displayManager.applied();
    const auto paintOutgoing = [this](render::Renderer &renderer) {
        renderer.clear(scene.background());
        for (auto *layer : transitionLayers)
            layer->render(renderer);
        screenTransition->render(renderer);
    };

    scene::ScreenTransitionNode::Outputs rotated;
    QList<QRect> outgoing;
    QList<QRect> covers;
    for (int index = 0; index < outputModels.size() && index < int(backend->allOutputs().size());
         ++index) {
        auto *model = outputModels.at(index);
        // Only a published model has a frame on screen to hold.
        if (model->logicalExtent.isEmpty())
            continue;
        const DisplayId destination(model->displayId);
        const LogicalDisplay *logical = nullptr;
        for (const auto &candidate : applied.logicalDisplays) {
            if (candidate.destinations.contains(destination)) {
                logical = &candidate;
                break;
            }
        }
        const int quarterTurns = logical ? int(logical->rotation) - int(model->transform) : 0;
        if (quarterTurns == 0)
            continue;
        const QRect outgoingBounds(model->position, model->logicalSize());
        auto texture = backend->allOutputs()[index]->renderOffscreen(
                {model->name, outgoingBounds, model->pixelSize, model->renderScale,
                 static_cast<uint32_t>(model->transform), 0, model->nativeContentRect},
                paintOutgoing);
        if (!texture)
            continue;
        // Renderer transforms turn logical content by -90 degrees per step, so
        // the outgoing frame stays put on the panel when turned by the difference.
        static constexpr int kDegrees[] = {0, 90, -180, -90};
        rotated[model->name] = {std::move(texture), kDegrees[(quarterTurns % 4 + 4) % 4]};
        outgoing.append(outgoingBounds);
        covers.append(logical->bounds);
    }
    if (rotated.empty())
        return;

    // Upstream extends the compositor lock until the wallpaper and apps have
    // resized. Here the shell's surfaces on a rotated output have to commit
    // again, and every desktop has to cover its output.
    struct TrackedSurface {
        quint64 window;
        uint64_t revision;
    };

    std::vector<TrackedSurface> tracked;
    bool hasDesktop = false;
    for (auto *window : windowManager.windows()) {
        const auto role = window->role();
        if (role != wm::WindowRole::Desktop && role != wm::WindowRole::Panel
            && role != wm::WindowRole::Dock)
            continue;
        const auto *node = window->rootSurface() ? window->rootSurface()->node() : nullptr;
        if (!node || !node->isEffectivelyVisible())
            continue;
        hasDesktop |= role == wm::WindowRole::Desktop;
        if (std::any_of(outgoing.begin(), outgoing.end(), [&](const QRect &bounds) {
                return node->absoluteGeometry().intersects(bounds);
            }))
            tracked.push_back({window->id(), node->contentRevision()});
    }
    if (!hasDesktop)
        covers.clear();
    const auto ready = [this, tracked = std::move(tracked), covers = std::move(covers)] {
        for (const auto &surface : tracked) {
            auto *window = windowManager.windowById(surface.window);
            const auto *node =
                    window && window->rootSurface() ? window->rootSurface()->node() : nullptr;
            if (node && node->isEffectivelyVisible() && node->contentRevision() == surface.revision)
                return false;
        }
        for (const QRect &bounds : covers) {
            const auto &windows = windowManager.windows();
            if (std::none_of(windows.begin(), windows.end(), [&](const wm::Window *window) {
                    const auto *node =
                            window->rootSurface() ? window->rootSurface()->node() : nullptr;
                    return window->role() == wm::WindowRole::Desktop && node
                            && node->isEffectivelyVisible()
                            && node->absoluteGeometry().contains(bounds);
                }))
                return false;
        }
        return true;
    };
    screenTransition->start(
            std::move(rotated),
            [this](render::Renderer &renderer) {
                renderer.clear(scene.background());
                for (auto *layer : transitionLayers)
                    layer->render(renderer);
            },
            ready, kScreenTransitionTimeoutMs);
    transitionDeadline.start(kScreenTransitionTimeoutMs);
}

void DisplayRuntime::setStartupCover(scene::ScreenTransitionNode *cover, QList<scene::Node *> layers,
                                     std::function<bool()> ready)
{
    startupCover = cover;
    startupCoverLayers = std::move(layers);
    startupCoverReady = std::move(ready);
    startupCoverPending = cover != nullptr;
    if (!backend)
        return;
    for (auto *output : backend->allOutputs())
        output->setKeepCarriedFrame(startupCoverPending);
}

bool DisplayRuntime::isStartupCoverShowing() const
{
    return startupCover && backend && (startupCoverPending || startupCover->isActive());
}

void DisplayRuntime::startStartupCover()
{
    const auto &applied = *displayManager.applied();
    // A reconciliation publishes an empty layout before it configures: only
    // the first real one has carried frames, and only it gets the cover. A
    // later one, a VT switch back included, shows the session at once.
    if (!startupCoverPending || !startupCover || !backend || applied.logicalDisplays.isEmpty())
        return;
    startupCoverPending = false;
    const auto outputs = backend->allOutputs();
    scene::ScreenTransitionNode::Outputs carried;
    QList<QRect> covers;
    for (int index = 0; index < int(outputs.size()); ++index) {
        outputs[index]->setKeepCarriedFrame(false);
        auto frame = outputs[index]->takeCarriedFrame();
        if (!frame || index >= outputModels.size())
            continue;
        const auto *model = outputModels.at(index);
        const DisplayId destination(model->displayId);
        const auto logical = std::find_if(applied.logicalDisplays.begin(),
                                          applied.logicalDisplays.end(), [&](const auto &candidate) {
                                              return candidate.destinations.contains(destination);
                                          });
        if (logical == applied.logicalDisplays.end())
            continue;
        // The carried framebuffer is the panel's, unturned; like an outgoing
        // frame, it stays put when turned by the new rotation.
        static constexpr int kDegrees[] = {0, 90, -180, -90};
        carried[model->name] = {std::move(frame), kDegrees[int(logical->rotation) % 4]};
        covers.append(logical->bounds);
    }
    if (carried.empty())
        return;
    auto desktopsCover = [this, covers = std::move(covers)] {
        const auto &windows = windowManager.windows();
        return std::all_of(covers.begin(), covers.end(), [&](const QRect &bounds) {
            return std::any_of(windows.begin(), windows.end(), [&](const wm::Window *window) {
                const auto *node = window->rootSurface() ? window->rootSurface()->node() : nullptr;
                return window->role() == wm::WindowRole::Desktop && node
                        && node->isEffectivelyVisible() && node->absoluteGeometry().contains(bounds);
            });
        });
    };
    startupCover->start(
            std::move(carried),
            [this](render::Renderer &renderer) {
                renderer.clear(scene.background());
                for (auto *layer : startupCoverLayers)
                    layer->render(renderer);
            },
            startupCoverReady ? startupCoverReady : std::function<bool()>(std::move(desktopsCover)),
            kStartupCoverTimeoutMs);
    startupCoverDeadline.start(kStartupCoverTimeoutMs);
}

void DisplayRuntime::bindFrames()
{
    if (!backend)
        return;
    for (int index = 0; index < int(backend->allOutputs().size()); ++index) {
        auto *output = backend->allOutputs()[index];
        if (boundOutputs.contains(output))
            continue;
        boundOutputs.insert(output);
        QObject::connect(output, &QObject::destroyed, &displayManager, [this, output] {
            boundOutputs.remove(output);
        });
        auto *model = outputModels.at(index);
        auto tokens = std::make_shared<QHash<quint64, quint64>>();
        output->presentationOutput = model->presentation;
        output->setDirectScanoutEnabled(true);
        output->setSecureScanout(secureScanout);
        output->setSecureContentCheck([this](double frameTime) {
            return !secureContentSettled || secureContentSettled(frameTime);
        });
        output->setKeepCarriedFrame(startupCoverPending);
        output->setCarryOverWhileSecure(carryOverWhileSecure);
        output->setScanoutPlanner([this, model](const QSize &size, size_t maxOverlays) {
            const auto &applied = displayManager.applied();
            if (!applied || applied->logicalDisplays.size() != 1
                || applied->logicalDisplays.front().destinations.size() != 1
                || (capturePending && capturePending(model->name)) || !model->position.isNull()
                || model->transform != compositor::Output::Transform::Normal
                || !qFuzzyCompare(model->renderScale, model->scale.value()))
                return scene::OverlayPlan();
            return scene::proposeOverlays(*scene.root(), size, maxOverlays, model->scale.value());
        });
        output->setBeginFrameCallback([this, model](double frameTime) {
            Q_EMIT compositor.frameStarted(frameTime);
            // Like viz's Display without damage, an output that shows nothing
            // new and owes no client a frame draws and flips nothing.
            const QRect viewport(model->position, model->logicalSize());
            return scene.hasDamage({model->name, viewport, model->pixelSize, model->renderScale,
                                    static_cast<uint32_t>(model->transform), 0,
                                    model->nativeContentRect})
                    || compositor.awaitsFrame(viewport)
                    || (capturePending && capturePending(model->name));
        });
        output->setPrepareCallback([this, model, tokens](render::Renderer &renderer,
                                                         quint64 generation) {
            const auto token = ++nextFrameToken;
            tokens->insert(generation, token);
            renderer.setOutputContext({model->name, QRect(model->position, model->logicalSize()),
                                       model->pixelSize, model->renderScale,
                                       static_cast<uint32_t>(model->transform), token,
                                       model->nativeContentRect});
            compositor.updateTextures(renderer);
            compositor.frameSubmittedForOutput(QRect(model->position, model->logicalSize()), token);
        });
        output->setRenderCallback([this](render::Renderer &renderer, quint64) {
            scene.render(renderer);
            Q_EMIT compositor.outputRendered(renderer);
        });
        output->presentationSubmitted = [this, model](const auto &submission) {
            compositor.abandonUnsubmittedPresentation(
                    QRect(model->position, model->logicalSize()), submission);
        };
        QObject::connect(output, &backend::DrmOutput::imagePresented, &displayManager,
                         [this, tokens](quint64 generation, quint64 timeNs) {
                             if (tokens->contains(generation))
                                 Q_EMIT compositor.outputPresented(tokens->value(generation),
                                                                   timeNs);
                         });
        // exo runs wl_surface.frame on the ack viz sends from
        // Display::DrawAndSwap (RunDrawCallbacks), not on presentation. Paced
        // by flips, a client behind a queued frame draws every other refresh.
        QObject::connect(output, &backend::DrmOutput::frameDrawn, &displayManager,
                         [this, tokens](quint64 generation, quint64 aimedNs) {
                             if (!tokens->contains(generation))
                                 return;
                             compositor.frameDrawn(wayland::nowInMilliseconds(),
                                                   tokens->value(generation));
                             Q_EMIT compositor.outputDrawn(tokens->value(generation), aimedNs);
                         });
        QObject::connect(output, &backend::DrmOutput::framePresented, &displayManager,
                         [this, tokens, model, output](quint32, quint64 generation) {
                             if (!tokens->remove(generation))
                                 return;
                             if (securelyPresented && output->lastPresentedFrameWasSecure())
                                 securelyPresented(model->displayId);
                         });
        QObject::connect(output, &backend::DrmOutput::frameDiscarded, &displayManager,
                         [this, tokens](quint64 generation) {
                             if (tokens->contains(generation))
                                 compositor.frameDiscarded(tokens->take(generation));
                         });
        QObject::connect(&compositor, &compositor::Compositor::repaintNeeded, output,
                         [output](bool decoration) {
                             output->scheduleFrame(decoration
                                                           ? backend::DrmOutput::Reason::Decoration
                                                           : backend::DrmOutput::Reason::Surface);
                         });
    }
}

ConfigurationResult DisplayRuntime::configure(const DisplayConfiguration &configuration,
                                              NativeDisplayDelegate::ConfigureMode mode)
{
    const auto exact = [&](const DisplayConfiguration &candidate,
                           NativeDisplayDelegate::ConfigureMode phase) {
        auto result = nativeConfigure ? nativeConfigure(candidate, phase)
                                      : configureExact(candidate, phase);
        if (!result.succeeded && result.error.code == DisplayErrorCode::RevertFailed
            && displayManager.applied())
            requireNativeRecovery();
        return result;
    };
    if (!configuration.allowModeFallback || mode != NativeDisplayDelegate::ConfigureMode::TestOnly)
        return exact(configuration, mode);
    if (!displayManager.hardware())
        return {false,
                false,
                {DisplayErrorCode::Unavailable, QStringLiteral("No hardware snapshot"), {}},
                std::nullopt};
    const auto hardware = *displayManager.hardware();
    const uint64_t revision = displayManager.applied() ? displayManager.applied()->revision : 0;
    const auto validated =
            DisplayConfigurationValidator::validateExtended(hardware, revision, configuration);
    if (!validated.succeeded)
        return {false, false, validated.error, std::nullopt};
    QList<backend::DrmFallbackHead> heads;
    for (const auto &request : validated.normalized.displays) {
        const auto physical = std::find_if(hardware.displays.begin(), hardware.displays.end(),
                                           [&](const auto &item) {
                                               return item.sessionId == request.display;
                                           });
        backend::DrmFallbackHead head;
        head.connectorName = physical->connectorName;
        head.baseConnectorKey =
                physical->devicePath + QLatin1Char(':') + physical->baseConnectorKey;
        head.internal =
                !request.enabled || physical->connectionType != DisplayConnectionType::External;
        const auto append = [&](const DisplayMode &candidate, bool preferred) {
            head.modes.append({candidate.stableKey(), candidate.pixelSize.width(),
                               candidate.pixelSize.height(), candidate.refreshMilliHz,
                               candidate.pixelClockKHz, preferred});
        };
        append(request.mode, true);
        if (!head.internal) {
            for (const auto &candidate : physical->modes) {
                if (candidate.pixelClockKHz > 0
                    && candidate.pixelClockKHz < request.mode.pixelClockKHz
                    && candidate.pixelSize.width() <= request.mode.pixelSize.width()
                    && candidate.pixelSize.height() <= request.mode.pixelSize.height()
                    && candidate.refreshMilliHz <= request.mode.refreshMilliHz)
                    append(candidate, false);
            }
        }
        heads.append(head);
    }
    ConfigurationResult last;
    DisplayConfiguration selected;
    const uint64_t epoch = backend ? backend->topologyEpoch() : 0;
    int testRounds = 0;
    QStringList diagnostics;
    auto plan = backend::DrmModeFallbackPlanner::choose(
            heads,
            [&](const auto &modes) {
                const auto current = [&] {
                    return displayManager.hardware()
                            && displayManager.hardware()->topologyGeneration
                            == hardware.topologyGeneration
                            && (!backend || backend->topologyEpoch() == epoch);
                };
                if (testRounds >= 7)
                    return backend::DrmFallbackTestResult {false, {}, last.error.message, false};
                if (!current()) {
                    last = {false,
                            false,
                            {DisplayErrorCode::StaleConfiguration,
                             QStringLiteral("Hardware invalidated during recovery testing"),
                             {}},
                            std::nullopt};
                    return backend::DrmFallbackTestResult {false, {}, last.error.message, false};
                }
                selected = validated.normalized;
                selected.allowModeFallback = false;
                for (int i = 0; i < modes.size(); ++i) {
                    auto &request = selected.displays[i];
                    for (const auto &physical : hardware.displays) {
                        if (physical.sessionId != request.display)
                            continue;
                        for (const auto &candidate : physical.modes)
                            if (candidate.stableKey() == modes[i].key)
                                request.mode = candidate;
                    }
                }
                const auto checked = DisplayConfigurationValidator::validateExtended(
                        hardware, revision, selected);
                if (!checked.succeeded) {
                    last = {false, false, checked.error, std::nullopt};
                    return backend::DrmFallbackTestResult {false, {}, last.error.message};
                }
                selected = checked.normalized;
                ++testRounds;
                last = exact(selected, NativeDisplayDelegate::ConfigureMode::TestOnly);
                int resources = 0;
                while (!last.succeeded && last.error.code == DisplayErrorCode::TestFailed
                       && current() && testRounds < 7 && resources < 3 && backend
                       && backend->tryNextConfigurationResources()) {
                    if (!current())
                        break;
                    bindFrames();
                    ++resources;
                    ++testRounds;
                    qCInfo(cfCompositor)
                            << "Retrying native route/buffer allocation, round" << testRounds;
                    last = exact(selected, NativeDisplayDelegate::ConfigureMode::TestOnly);
                }
                QStringList keys;
                for (int i = 0; i < modes.size(); ++i)
                    keys.append(heads[i].connectorName + QLatin1Char('=') + modes[i].key);
                diagnostics.append(QStringLiteral("round %1 [%2]: %3")
                                           .arg(testRounds)
                                           .arg(keys.join(QLatin1Char(',')),
                                                last.succeeded ? QStringLiteral("passed")
                                                               : last.error.message));
                qCInfo(cfCompositor).noquote() << "Display recovery" << diagnostics.last();
                if (!current())
                    last = {false,
                            false,
                            {DisplayErrorCode::StaleConfiguration,
                             QStringLiteral("Hardware invalidated during recovery testing"),
                             {}},
                            std::nullopt};
                QStringList failedGroups;
                for (const auto &id : last.error.affectedDisplays) {
                    for (const auto &physical : hardware.displays)
                        if (physical.sessionId == id)
                            failedGroups.append(physical.devicePath + QLatin1Char(':')
                                                + physical.baseConnectorKey);
                }
                return backend::DrmFallbackTestResult {
                        last.succeeded, failedGroups, last.error.message,
                        last.error.code == DisplayErrorCode::TestFailed};
            },
            7);
    if (!plan && last.error.code == DisplayErrorCode::TestFailed && testRounds < 8
        && validated.normalized.displays.size() > 1 && displayManager.hardware()
        && displayManager.hardware()->topologyGeneration == hardware.topologyGeneration
        && (!backend || backend->topologyEpoch() == epoch)) {
        selected = validated.normalized;
        selected.allowModeFallback = false;
        DisplayId survivor = selected.primary;
        for (const auto &physical : hardware.displays)
            if (physical.connectionType == DisplayConnectionType::Internal)
                for (const auto &request : selected.displays)
                    if (request.enabled && request.display == physical.sessionId)
                        survivor = physical.sessionId;
        selected.primary = survivor;
        selected.placements.clear();
        selected.mirrorGroups.clear();
        QStringList disabled;
        for (auto &request : selected.displays) {
            if (request.display != survivor && request.enabled)
                disabled.append(request.display.value());
            request.enabled = request.display == survivor;
        }
        if (!disabled.isEmpty()) {
            const auto checked =
                    DisplayConfigurationValidator::validateExtended(hardware, revision, selected);
            if (checked.succeeded) {
                selected = checked.normalized;
                ++testRounds;
                qCWarning(cfCompositor) << "Testing single-output rescue on" << survivor.value();
                last = exact(selected, NativeDisplayDelegate::ConfigureMode::TestOnly);
                if (!displayManager.hardware()
                    || displayManager.hardware()->topologyGeneration != hardware.topologyGeneration
                    || (backend && backend->topologyEpoch() != epoch))
                    last = {false,
                            false,
                            {DisplayErrorCode::StaleConfiguration,
                             QStringLiteral("Hardware invalidated during single-output rescue"),
                             {}},
                            std::nullopt};
                if (last.succeeded)
                    plan = backend::DrmFallbackPlan {
                            {}, disabled, QStringLiteral("Single-output rescue"), testRounds};
            }
        }
    }
    if (!plan) {
        last.succeeded = false;
        last.error.message +=
                QStringLiteral("; recovery exhausted after %1 candidate rounds").arg(testRounds);
        return last;
    }
    last.selected = selected;
    last.degraded = !plan->downgradedConnectors.isEmpty();
    last.error.code = DisplayErrorCode::None;
    last.error.affectedDisplays.clear();
    for (int i = 0; i < selected.displays.size(); ++i)
        if (selected.displays[i].mode.stableKey()
                    != validated.normalized.displays[i].mode.stableKey()
            || selected.displays[i].enabled != validated.normalized.displays[i].enabled)
            last.error.affectedDisplays.append(selected.displays[i].display);
    if (last.degraded)
        last.error.message = QStringLiteral("Recovery selected a degraded configuration for %1 "
                                            "after %2 test rounds. %3")
                                     .arg(plan->downgradedConnectors.join(QLatin1Char(',')))
                                     .arg(testRounds)
                                     .arg(diagnostics.join(QStringLiteral("; ")));
    return last;
}

ConfigurationResult DisplayRuntime::configureExact(const DisplayConfiguration &configuration,
                                                   NativeDisplayDelegate::ConfigureMode mode)
{
    const uint64_t revision = displayManager.applied() ? displayManager.applied()->revision : 0;
    const auto result = cutefish::display::DisplayConfigurationValidator::validateExtended(
            *displayManager.hardware(), revision, configuration);
    if (!result.succeeded)
        return cutefish::display::ConfigurationResult {false, false, result.error, std::nullopt};
    if (backend) {
        std::vector<cutefish::backend::DrmOutput::Configuration> changes;
        bool changed = false;
        for (size_t index = 0; index < backend->allOutputs().size(); ++index) {
            auto *output = backend->allOutputs()[index];
            const cutefish::display::DisplayId id(outputModels.at(index)->displayId);
            for (const auto &request : configuration.displays) {
                if (request.display != id)
                    continue;
                auto selected = output->mode();
                bool found = !request.enabled;
                for (const auto &native : output->modes()) {
                    if (QSize(native.hdisplay, native.vdisplay) == request.mode.pixelSize
                        && native.clock == request.mode.pixelClockKHz
                        && native.htotal == request.mode.horizontalTotal
                        && native.vtotal == request.mode.verticalTotal
                        && native.flags == request.mode.flags) {
                        selected = native;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return cutefish::display::ConfigurationResult {
                            false,
                            false,
                            {cutefish::display::DisplayErrorCode::UnsupportedMode,
                             QStringLiteral("The requested native mode is unavailable"),
                             {id}},
                            std::nullopt};
                const bool blanked =
                        request.enabled && isPoweredOff(id) && output->requiresSoftwareBlanking();
                const bool enabled = request.enabled && (!isPoweredOff(id) || blanked);
                changed |= output->isEnabled() != enabled || output->isBlanked() != blanked
                        || output->size() != request.mode.pixelSize
                        || output->mode().clock != selected.clock
                        || output->mode().flags != selected.flags
                        || output->mode().htotal != selected.htotal
                        || output->mode().vtotal != selected.vtotal;
                changes.push_back({output, selected, enabled, blanked});
            }
        }
        backend::DrmCommitStatus status = backend::DrmCommitStatus::Success;
        backend::DrmDeviceApi *failedDevice = nullptr;
        QString diagnostic;
        if (changed
            && !cutefish::backend::DrmOutput::configure(
                    changes,
                    mode == cutefish::display::NativeDisplayDelegate::ConfigureMode::TestOnly,
                    &status, &diagnostic, &failedDevice)) {
            QList<DisplayId> affected;
            const auto outputs = backend->allOutputs();
            for (int i = 0; i < outputs.size(); ++i)
                if (outputs[i]->backend()->device() == failedDevice)
                    affected.append(DisplayId(outputModels[i]->displayId));
            if (status == backend::DrmCommitStatus::RollbackFailed)
                requireNativeRecovery();
            return cutefish::display::ConfigurationResult {
                    false,
                    false,
                    {status == backend::DrmCommitStatus::RollbackFailed
                             ? DisplayErrorCode::RevertFailed
                             : status == backend::DrmCommitStatus::TestFailed
                                     || status == backend::DrmCommitStatus::TransferUnavailable
                             ? DisplayErrorCode::TestFailed
                             : status == backend::DrmCommitStatus::Invalidated
                             ? DisplayErrorCode::StaleConfiguration
                             : status == backend::DrmCommitStatus::PreparationFailed
                             ? DisplayErrorCode::ResourceExhausted
                             : DisplayErrorCode::CommitFailed,
                     diagnostic, affected},
                    std::nullopt};
        }
    }
    return cutefish::display::ConfigurationResult {true, false, {}, std::nullopt};
}

ConfigurationResult DisplayRuntime::setPower(const QList<DisplayId> &displays,
                                             NativeDisplayDelegate::PowerState state)
{
    if (!displayManager.applied())
        return {false,
                false,
                {DisplayErrorCode::Unavailable, QStringLiteral("No applied display state"), {}},
                std::nullopt};
    auto next = powerStates;
    for (const auto &id : displays) {
        bool known = false;
        for (const auto &physical : displayManager.applied()->physicalDisplays)
            known |= physical.sessionId == id;
        if (!known)
            return {false,
                    false,
                    {DisplayErrorCode::UnknownOutput, QStringLiteral("Unknown power target"), {id}},
                    std::nullopt};
    }
    for (const auto &physical : displayManager.applied()->physicalDisplays) {
        if (displays.isEmpty() || displays.contains(physical.sessionId))
            next.insert(physical.sessionId, state);
    }
    bool hasActiveOutput = false;
    bool allOff = true;
    for (const auto &logical : displayManager.applied()->logicalDisplays) {
        for (const auto &id : logical.destinations) {
            hasActiveOutput = true;
            allOff &= next.value(id, NativeDisplayDelegate::PowerState::On)
                    != NativeDisplayDelegate::PowerState::On;
        }
    }
    // Install the shield and secure KMS policy before powering off the last display.
    if (state != NativeDisplayDelegate::PowerState::On && hasActiveOutput && allOff
        && lockRequested)
        lockRequested();
    if (backend) {
        std::vector<backend::DrmOutput::Configuration> changes;
        for (size_t index = 0; index < backend->allOutputs().size(); ++index) {
            const DisplayId id(outputModels.at(index)->displayId);
            auto *output = backend->allOutputs()[index];
            bool active = false;
            for (const auto &logical : displayManager.applied()->logicalDisplays)
                active |= logical.destinations.contains(id);
            auto selected = output->mode();
            for (const auto &physical : displayManager.applied()->physicalDisplays) {
                if (physical.sessionId != id || !physical.currentMode)
                    continue;
                bool found = false;
                for (const auto &native : output->modes()) {
                    const auto &mode = *physical.currentMode;
                    if (QSize(native.hdisplay, native.vdisplay) == mode.pixelSize
                        && native.clock == mode.pixelClockKHz
                        && native.htotal == mode.horizontalTotal
                        && native.vtotal == mode.verticalTotal && native.flags == mode.flags) {
                        selected = native;
                        found = true;
                        break;
                    }
                }
                if (active && !found)
                    return {false,
                            false,
                            {DisplayErrorCode::UnsupportedMode,
                             QStringLiteral("The applied native mode is unavailable"),
                             {id}},
                            std::nullopt};
            }
            const bool off = next.value(id, NativeDisplayDelegate::PowerState::On)
                    != NativeDisplayDelegate::PowerState::On;
            // Virtual scanouts need a black framebuffer instead of a detached CRTC.
            const bool blanked = active && off && output->requiresSoftwareBlanking();
            changes.push_back({output, selected, active && (!off || blanked), blanked});
        }
        backend::DrmCommitStatus status;
        QString diagnostic;
        if (!backend::DrmOutput::configure(changes, false, &status, &diagnostic)) {
            if (status == backend::DrmCommitStatus::RollbackFailed)
                requireNativeRecovery();
            return {false,
                    false,
                    {status == backend::DrmCommitStatus::RollbackFailed
                             ? DisplayErrorCode::RevertFailed
                             : DisplayErrorCode::CommitFailed,
                     diagnostic,
                     {}},
                    std::nullopt};
        }
    }
    for (const auto &physical : displayManager.applied()->physicalDisplays) {
        if (!displays.isEmpty() && !displays.contains(physical.sessionId))
            continue;
        if (state == NativeDisplayDelegate::PowerState::On)
            wakeOnInput.remove(physical.sessionId);
        // Explicit per-display power-off also serves the closed-lid policy.
        else if (displays.isEmpty() && !isPoweredOff(physical.sessionId))
            wakeOnInput.insert(physical.sessionId);
    }
    powerStates = std::move(next);
    // With every display off nothing draws, so closing windows are done now
    // rather than kept, snapshot and all, until a display comes back.
    if (std::all_of(displayManager.applied()->physicalDisplays.begin(),
                    displayManager.applied()->physicalDisplays.end(),
                    [this](const auto &physical) { return isPoweredOff(physical.sessionId); }))
        scene.retireClosing();
    if (backend && secureScanout && securelyPresented) {
        for (size_t index = 0; index < backend->allOutputs().size(); ++index) {
            if (backend->allOutputs()[index]->lastPresentedFrameWasSecure())
                securelyPresented(outputModels.at(index)->displayId);
        }
    }

    if (cursorRouter)
        cursorRouter->refresh();
    return {true, false, {}, displayManager.applied()};
}

void DisplayRuntime::requireNativeRecovery()
{
    displayManager.clearApplied(displayManager.hardware()->topologyGeneration);
    if (backend)
        QTimer::singleShot(0, backend, [native = backend] {
            emit native->outputsInvalidated();
        });
}

void DisplayRuntime::publish()
{
    if (!displayManager.applied()) {
        // Unpublished, not gone: the connectors are still there, so their
        // wl_output globals stay announced with their last metrics and are
        // brought up to date by the next publication. replaceOutputs()
        // withdraws the ones a reprobe finds missing.
        outputGlobal = nullptr;
        compositor.setOutputViews({});
        inputManager.setTopology({});
        windowManager.setOutputBounds({});
        scene.setSize({0, 0});
        cursor.setSink(nullptr);
        if (cursorRouter)
            cursorRouter->setTargets({});
        return;
    }
    captureOutgoingFrames();
    startStartupCover();
    updatingOutput = true;
    if (maximumScaleChanged) {
        double scale = 0;
        for (const auto &logical : displayManager.applied()->logicalDisplays)
            scale = std::max(scale, logical.scale.value());
        if (scale > 0)
            maximumScaleChanged(scale);
    }
    const auto &models = outputModels;
    // The topology coordinator's empty baseline, between a reprobe and the
    // configuration it reconciles: no display is disabled by it, so no global
    // is withdrawn for it.
    const bool baseline = displayManager.applied()->logicalDisplays.isEmpty();
    QList<cutefish::compositor::Compositor::OutputView> views;
    QList<QRect> topology;
    QSet<QString> logicalViews;
    QRect desktop;
    for (int index = 0; index < models.size(); ++index) {
        auto *model = models.at(index);
        const cutefish::display::DisplayId destination(models.at(index)->displayId);
        for (const auto &physical : displayManager.applied()->physicalDisplays) {
            if (physical.sessionId == destination && physical.currentMode) {
                model->pixelSize = physical.currentMode->pixelSize;
                model->refreshMilliHz = physical.currentMode->refreshMilliHz;
                model->preferred = physical.currentMode->preferred;
            }
        }
        const cutefish::display::LogicalDisplay *logical = nullptr;
        for (const auto &candidate : displayManager.applied()->logicalDisplays) {
            if (candidate.destinations.contains(destination)) {
                logical = &candidate;
                break;
            }
        }
        const bool published = logical && logical->destinations.first() == destination;
        if (backend && index < int(backend->allOutputs().size()))
            backend->allOutputs()[index]->setDirectScanoutEnabled(
                    published && logical->primary
                    && displayManager.applied()->logicalDisplays.size() == 1
                    && logical->destinations.size() == 1);
        if (!published && !baseline && index < int(outputGlobals.size()) && outputGlobals[index]) {
            if (outputGlobal == outputGlobals[index])
                outputGlobal = nullptr;
            cutefish::wayland::removeOutputGlobal(outputGlobals[index]);
            outputGlobals[index] = nullptr;
        }
        if (!logical)
            continue;
        model->logicalExtent = logical->bounds.size();
        model->position = logical->bounds.topLeft();
        model->scale = logical->scale;
        model->transform = static_cast<cutefish::compositor::Output::Transform>(logical->rotation);
        const auto mapping = displayManager.mappingFor(destination);
        Q_ASSERT(mapping);
        model->renderScale = mapping->renderScale;
        model->nativeContentRect = mapping->nativeContent;
        if (!logicalViews.contains(logical->id.value())) {
            views.append({logical->id.value(), logical->bounds, logical->scale, model->name});
            topology.append(logical->bounds);
            logicalViews.insert(logical->id.value());
        }
        desktop = desktop.united(logical->bounds);
        if (published && index < int(outputGlobals.size()) && !outputGlobals[index])
            outputGlobals[index] = cutefish::wayland::createOutputGlobal(waylandDisplay, model);
        if (published && index < int(outputGlobals.size()))
            cutefish::wayland::updateOutputGlobal(outputGlobals[index], model);
    }
    for (auto *global : outputGlobals) {
        if (global) {
            outputGlobal = global;
            break;
        }
    }
    compositor.setOutputViews(std::move(views));
    QRect primary;
    for (const auto &logical : displayManager.applied()->logicalDisplays) {
        if (logical.primary)
            primary = logical.bounds;
    }
    inputManager.setTopology(topology, primary);
    windowManager.setOutputBounds(desktop);
    scene.setSize(desktop.size());
    if (screenTransition)
        screenTransition->setGeometry(QRect(QPoint(), desktop.size()));
    if (startupCover)
        startupCover->setGeometry(QRect(QPoint(), desktop.size()));
    if (cursorRouter && backend) {
        std::vector<cutefish::cursor::OutputCursorRouter::Target> targets;
        for (int index = 0; index < models.size() && index < int(backend->allOutputs().size());
             ++index) {
            const cutefish::display::DisplayId destination(models.at(index)->displayId);
            const cutefish::display::LogicalDisplay *logical = nullptr;
            for (const auto &candidate : displayManager.applied()->logicalDisplays) {
                if (candidate.destinations.contains(destination)) {
                    logical = &candidate;
                    break;
                }
            }
            if (!logical)
                continue;
            targets.push_back({logical->bounds, backend->allOutputs()[index],
                               displayManager.mappingFor(destination)->transform()});
        }
        cursorRouter->setTargets(std::move(targets));
        const auto *primary = displayManager.primaryDisplay();
        bool uniform = primary != nullptr;
        for (const auto &logical : displayManager.applied()->logicalDisplays) {
            uniform &= primary && logical.scale == primary->scale
                    && logical.rotation == cutefish::display::DisplayRotation::Normal
                    && logical.destinations.size() == 1;
        }
        cursor.setSink(uniform ? cursorRouter : nullptr);
        if (primary)
            cursor.setScale(primary->scale.value());
    }
    updatingOutput = false;
    for (auto *global : outputGlobals)
        cutefish::wayland::updateOutputMembership(global);
}
} // namespace cutefish::display
