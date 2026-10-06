// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WM_WINDOW_MANAGER_H_
#define CUTEFISH_WM_WINDOW_MANAGER_H_

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <QObject>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>

#include "decoration/window_visual_style.h"
#include "input/event_sink.h"
#include "wm/activation.h"
#include "wm/placement.h"
#include "wm/stacking.h"
#include "wm/types.h"
#include "wm/window_frame.h"
#include "wm/window_modality_controller.h"
#include "wm/work_area.h"

namespace cutefish::compositor {
class Compositor;
class Surface;
} // namespace cutefish::compositor

namespace cutefish::input {
class Seat;
class SessionGate;
} // namespace cutefish::input

namespace cutefish::display {
class DisplayManager;
}

namespace cutefish::scene {
class Node;
}

namespace cutefish::wm {

class DesksController;
class MruWindowTracker;
class OverviewController;
class ShellSurface;
class ToplevelEventHandler;
class TransientManager;
class Window;

// What the trusted shell said about a surface through cutefish_shell_v1,
// already adapted into window-manager concepts. The protocol layer registers it
// against the wl_surface's compositor::Surface; the window manager applies it
// when the surface's xdg role turns into a Window, before placement and before
// the window is announced.
struct ShellWindowMetadata {
    WindowRole role = WindowRole::Normal;
    FocusPolicy focusPolicy = FocusPolicy::Normal;
    PanelBehavior panelBehavior = PanelBehavior::StayAbove;
    QPoint position;
    bool hasPosition = false;
    QString outputName;
    // The window a Sheet hangs from, by Window::id; zero for none.
    quint64 parentWindow = 0;
};

// Everything a window is announced with. The application id and the process id
// belong here rather than being set afterwards: windowAdded is what the
// window-management protocol enumerates from, and a task that appears without
// an app id never gets one, because nothing changed after that.
struct WindowInit {
    WindowType type = WindowType::Normal;
    QPoint origin;
    QSize requestedSize;
    QString title;
    QString applicationId;
    qint64 processId = 0;
    bool decorated = false;
    QSize minimumSize {1, 1};
    QSize maximumSize;
    FocusPolicy focusPolicy = FocusPolicy::Normal;
    // A transient toplevel is centered over its parent instead of placed.
    Window *transientParent = nullptr;
    // Cascades the window from the application's most recent one instead of
    // centering it. See window_positioner.h.
    bool positionManaged = false;
    // Window-modal on `transientParent`, applied before anything can activate
    // the window.
    bool modal = false;
    // The output an ordinary window is placed on, chosen when it was first
    // configured. Empty means outputForNewWindow() at creation.
    QRect output;
    std::function<void()> closeCallback;
};

// Owns the windows, their stacking order, and the policy that decides which of
// them is active. It is also the compositor's input sink: it sees pointer and
// key events before the seat dispatches them to clients, so that a drag can
// consume them and a click can change activation first.
class WindowManager : public QObject, public input::EventSink, public ActivationObserver
{
    Q_OBJECT

public:
    // `seat` may be null in tests that do not exercise keyboard focus.
    WindowManager(compositor::Compositor *compositor, input::Seat *seat, QObject *parent = nullptr);
    ~WindowManager() override;

    compositor::Compositor *compositor() const { return m_compositor; }

    input::Seat *seat() const { return m_seat; }

    // Where server-side frames come from. Null leaves every window
    // undecorated, which is what the protocol-only tests want. Set once, before
    // any window exists.
    FrameProvider *frameProvider() const { return m_frameProvider; }

    void setFrameProvider(FrameProvider *provider) { m_frameProvider = provider; }

    const decoration::WindowVisualStyle &visualStyle() const { return m_visualStyle; }

    void setVisualStyle(const decoration::WindowVisualStyle &style);

    // The session's view of whether the desktop may be touched at all. Null
    // leaves the window manager unrestricted, which is what tests that never
    // lock the session want.
    void setSessionGate(input::SessionGate *gate) { m_sessionGate = gate; }

    input::SessionGate *sessionGate() const { return m_sessionGate; }

    // Whether any window may take activation, and the scene subtree input may
    // target. Both answer for the session, not for a single window.
    bool allowsActivation() const;
    scene::Node *inputRoot() const;

    Activation *activation() { return &m_activation; }

    // Gives `window` the keyboard without making it active, as Ash's capture
    // session filters keys while the application window keeps activation. It
    // lasts until the window is hidden or gone.
    void setKeyboardGrab(Window *window);

    TransientManager *transientManager() { return m_transientManager.get(); }

    MruWindowTracker *mruWindowTracker() const { return m_mruTracker.get(); }

    DesksController *desks() const { return m_desks.get(); }

    OverviewController *overview() const { return m_overview.get(); }

    ToplevelEventHandler *eventHandler() { return m_eventHandler.get(); }

    // --- Windows ---

    // The window takes its initial bounds from the placement policy, using
    // `init.requestedSize` when it is not empty and the surface's current
    // content size otherwise. Both describe the client area: the frame is added
    // around it when `init.decorated` asks for one.
    Window *createWindow(compositor::Surface *rootSurface, const WindowInit &init = {});
    void destroyWindow(Window *window);

    // Bottom to top.
    const std::vector<Window *> &windows() const { return m_stackingOrder; }

    // Decides from the client's pid whether its toplevels are system dialogs.
    // The pid comes from the socket, so it names the process, not a claim.
    void setSystemDialogPolicy(std::function<bool(qint64 pid)> policy)
    {
        m_systemDialogPolicy = std::move(policy);
    }

    // Moves `window` to the top of its own stacking layer, in the model and in
    // the scene. A raise never lifts a window past a window of a higher layer:
    // an ordinary application can no more cover the dock than a dock can bury
    // the notifications.
    void raise(Window *window);

    // To the bottom of the window's stacking layer, its transient children
    // still above it.
    void stackAtBottomOfLayer(Window *window);

    // Synchronizes live scene order, preserving outgoing snapshot slots.
    void restackScene();

    // The window owning `surface`, following the surface hierarchy up to its
    // root first. A subsurface therefore resolves to its toplevel's window.
    Window *windowForSurface(compositor::Surface *surface) const;

    // --- Applications ---
    //
    // Quit, Hide and Hide Others, for the shell's application menu and dock
    // (cutefish_window_management_v1) and the Command convention's shortcuts.
    // An application is the task-list windows of one application id, as the
    // dock groups them, or of one process for windows without an id.

    // Asks the application's top-level windows to close; dialogs are left
    // alone, so a second Quit asks again rather than dismissing a prompt. Their
    // processes that have no task-list window left after the grace period are
    // terminated through setTerminateProcess().
    void quitApplication(Window *window);

    // Whether `candidate` is one of `window`'s application's windows, as Quit
    // and the dock group them.
    bool belongsToApplication(const Window *candidate, const Window *window) const;

    void hideApplication(Window *window);

    // Shows every hidden window of `window`'s application again, in place:
    // what switching to a hidden application does.
    void unhideApplication(Window *window);

    // Hides every application but `window`'s; all of them for null.
    void hideOtherApplications(Window *window);

    // How Quit ends a process that closed its windows but keeps running: the
    // session's AppRuntime in production. Unset, the process is left alone.
    void setTerminateProcess(std::function<void(qint64 pid)> terminate)
    {
        m_terminateProcess = std::move(terminate);
    }

    void setQuitGracePeriod(std::chrono::milliseconds period) { m_quitGracePeriod = period; }

    // Ends `window`'s client at once, without asking it, and the rest of the
    // application through setForceQuitProcess().
    void forceQuit(Window *window);

    // How force quit also ends the application scope the process was started
    // in: the session's AppRuntime in production. Unset, only the client ends.
    void setForceQuitProcess(std::function<void(qint64 pid)> forceQuit)
    {
        m_forceQuitProcess = std::move(forceQuit);
    }

    // How the user is offered force quit for a window that stopped answering;
    // returns whether anybody offered it. The window-management protocol's
    // not_responding in production.
    void setNotRespondingPrompt(std::function<bool(Window *)> prompt)
    {
        m_notRespondingPrompt = std::move(prompt);
    }

    // Called by Window; not part of the public API.
    bool promptNotResponding(Window *window);

    // The window with this identifier, or null. Identifiers are stable for the
    // life of the compositor and never reused.
    Window *windowById(quint64 id) const;

    // --- Private shell metadata ---

    // Records what the trusted shell said about `surface` and, when the surface
    // already has a window, applies it there. Passing metadata whose role is
    // Normal is how the protocol layer takes a surface back out of the shell
    // model.
    void setShellMetadata(compositor::Surface *surface, const ShellWindowMetadata &metadata);
    void clearShellMetadata(compositor::Surface *surface);
    const ShellWindowMetadata *shellMetadata(compositor::Surface *surface) const;

    // Re-sorts `window` into the stacking order after its role or panel
    // behavior changed. Called by Window; not part of the public API.
    void onWindowStackingChanged(Window *window);

    // Centres a Sheet on what its transient parent shows. Called by Window; not
    // part of the public API.
    void placeSheet(Window *sheet);

    // Called by Window and TransientManager; not part of the public API.
    void onWindowModalChanged(Window *window);
    void onWindowModalityInputsChanged(Window *window);
    void onWindowVisibilityChanged(Window *window);

    // --- Work-area reservations ---

    // `token` identifies the reservation's owner - the protocol layer uses the
    // shell surface object - so it can be withdrawn again. Every change
    // recomputes the work area from the output bounds.
    void setExclusiveZone(const void *token, ScreenEdge edge, int thickness,
                          const QString &outputName = QString());
    void clearExclusiveZone(const void *token);
    // What a shell disconnect does.
    void clearAllExclusiveZones();

    // --- Surfaces that are not windows ---

    // Auxiliary surfaces, such as Xwayland override-redirect surfaces, resolve
    // input and activation to their managed owner.
    void setSurfaceOwner(compositor::Surface *surface, Window *owner);
    void clearSurfaceOwner(compositor::Surface *surface);

    // Unowned auxiliary surfaces remain below trusted shell layers.
    void setAuxiliarySurface(compositor::Surface *surface, Window *owner);

    // --- Drag overlay ---

    // The snap preview an interactive move shows, stacked directly below
    // `below` so the dragged window stays on top of it. Passing null clears it.
    // The node is owned by whatever created it, not by the window manager.
    void setDragOverlay(scene::Node *overlay, Window *below);

    scene::Node *dragOverlay() const { return m_dragOverlay; }

    // Drawn right above the desks' windows, below everything that is on every
    // desk: the desk switch animation and the windows sent to another desk.
    void setDeskOverlays(std::vector<scene::Node *> overlays);

    // --- Popup grabs ---

    // The chain of popups holding an explicit grab, outermost first. A pointer
    // press outside the chain dismisses all of it and is not delivered.
    void pushPopupGrab(ShellSurface *popup);
    void removePopupGrab(ShellSurface *popup);

    const std::vector<ShellSurface *> &popupGrabs() const { return m_popupGrabs; }

    void dismissPopupGrabs();

    // Topmost visible window whose surface hierarchy contains `location`, given
    // in output coordinates. `surfaceLocation`, when non-null, receives the
    // point in the hit surface's own coordinates.
    Window *windowAt(const QPointF &location, compositor::Surface **hitSurface = nullptr,
                     QPointF *surfaceLocation = nullptr) const;

    // The topmost window shown at `location` as a capture sees the screen:
    // the capture interface is looked through, and a popup answers for the
    // window it belongs to.
    Window *capturableWindowAt(const QPointF &location) const;

    // The topmost window whose server-side frame owns `location`, and which
    // part of it. Null when the point belongs to a client, to nothing, or to a
    // window under one that already covers the point.
    Window *frameAt(const QPointF &location, FrameArea *area = nullptr) const;

    // The topmost window whose server resize handles own `location`, and the
    // edge they grab; see cutefish_window_hints_v1.set_server_resize. A touch
    // gets wider handles.
    Window *resizeHandleAt(const QPointF &location, bool touch, ResizeEdge *edge) const;

    // Whether this window is still managed. Callbacks are re-entrant, so a
    // pointer held across one has to be re-checked before it is used again.
    bool hasWindow(const Window *window) const;

    // --- Output and work area ---

    QRect outputBounds() const;
    QRect outputBoundsFor(const Window *window) const;
    void setWindowOutput(Window *window, const QString &waylandName);
    void clearWindowOutput(Window *window);
    void setOutputBounds(const QRect &bounds);
    void setDisplayManager(display::DisplayManager *manager);

    display::DisplayManager *displayManager() const { return m_displayManager; }

    // Where normal and maximized windows are laid out: the output bounds minus
    // whatever the trusted shell reserved through set_exclusive_zone. A shell
    // window is never confined to it - the launcher covers the whole screen -
    // so this is the output bounds for those.
    QRect workAreaFor(const Window *window) const;
    // What a normal window must stay partly inside while moved: the work area,
    // but down to the output's bottom edge so it can slide under the dock.
    QRect visibleAreaFor(const Window *window) const;
    void setWorkArea(const QRect &bounds);

    // The output bounds a new ordinary window goes to: the active application
    // window's output, else the one under the pointer, else the primary one.
    QRect outputForNewWindow() const;
    // The work area of the output with `outputBounds`, or an empty rect when
    // there is no such output any more.
    QRect workAreaForOutput(const QRect &outputBounds) const;

    struct OutputArea {
        QRect bounds;
        QRect workArea;
        double scale = 1;

        bool operator==(const OutputArea &) const = default;
    };
    // Every output, or the single output bounds without a display manager.
    std::vector<OutputArea> outputAreas() const;

    // --- input::EventSink ---

    bool onPointerMotion(input::Timestamp timestamp, const QPointF &location) override;
    bool onPointerButton(input::Timestamp timestamp, const QPointF &location, uint32_t evdevButton,
                         bool pressed) override;
    bool onPointerAxis(input::Timestamp timestamp, const QPointF &location, const QPointF &offset,
                       bool discrete) override;
    bool onPointerAxisStop(input::Timestamp timestamp, const QPointF &location) override;
    bool onKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed) override;
    bool onTouchDown(input::Timestamp timestamp, int32_t id, const QPointF &location) override;
    bool onTouchMotion(input::Timestamp timestamp, int32_t id, const QPointF &location) override;
    bool onTouchUp(input::Timestamp timestamp, int32_t id) override;
    void onTouchCancel(input::Timestamp timestamp) override;

    // --- ActivationObserver ---

    void onWindowActivated(Window *gainedActive, Window *lostActive) override;

Q_SIGNALS:
    void windowAdded(cutefish::wm::Window *window);
    void windowRemoved(cutefish::wm::Window *window);
    void activeWindowChanged(cutefish::wm::Window *window);
    // Every pointer motion, before any drag, grab or frame takes it: what the
    // dock's auto-hide watches, since it must see the pointer reach the screen
    // edge whatever else is going on.
    void pointerMoved(const QPointF &location);
    // What lies under a still pointer may have changed: a window was shown,
    // hidden, removed or restacked.
    void pointerTargetsChanged();
    // An output came, went or changed its bounds, work area or scale; windows
    // left on no output have been brought back by now.
    void outputsChanged();

private:
    bool hasApplicationWindow(qint64 process) const;

    // A press on a candidate popup picks a candidate: the window typed into
    // keeps the keyboard, and its menus stay open.
    bool isInputMethodPress(const QPointF &location) const;

    // Puts `window` at the top of its own layer in m_stackingOrder.
    void insertIntoStackingOrder(Window *window);
    void applyExclusiveZones();
    // Emits outputsChanged() when outputAreas() differs from the last time.
    void notifyOutputsChanged();
    bool isInsidePopupGrabChain(compositor::Surface *surface) const;
    bool isCaptureWindow(const Window *window) const;
    // The window a pointer event at `location` is aimed at, frames included,
    // when a modal transient blocks it.
    Window *modalBlockedWindowAt(const QPointF &location) const;
    bool handlePointerButton(input::Timestamp timestamp, const QPointF &location, uint32_t code,
                             bool pressed);
    bool isPopupSurface(compositor::Surface *surface) const;
    // A popup under `location` takes client input over any window handle.
    bool isPopupAt(const QPointF &location) const;
    // The frame currently holding a pressed caption button, if any.
    Window *windowTrackingFramePress() const;
    void setFrameHover(Window *window);
    // Click to activate, which also ends a keyboard grab the press is not in.
    void activateForPress(Window *window);
    // Hands the keyboard back to the active window.
    void releaseKeyboardGrab();
    // Hangs a Sheet from the application window `parentId`, or from nothing.
    void attachSheet(Window *sheet, quint64 parentId);
    // Where a window with a restore id is, kept as it unmaps and given back
    // to the next one with the same id; see set_restore_id.
    void rememberPlacement(const Window *window);
    bool restorePlacement(Window *window) const;

    compositor::Compositor *m_compositor = nullptr;
    input::Seat *m_seat = nullptr;
    input::SessionGate *m_sessionGate = nullptr;
    decoration::WindowVisualStyle m_visualStyle;
    FrameProvider *m_frameProvider = nullptr;
    // The window whose frame last saw the pointer, so it can be told when the
    // pointer leaves it.
    Window *m_frameHover = nullptr;
    scene::Node *m_dragOverlay = nullptr;
    Window *m_dragOverlayBelow = nullptr;

    std::vector<std::unique_ptr<Window>> m_windows;
    // Bottom to top. Mirrors m_windows but is reordered by raise().
    std::vector<Window *> m_stackingOrder;
    std::function<bool(qint64)> m_systemDialogPolicy;

    Activation m_activation;
    Window *m_keyboardGrab = nullptr;
    std::unique_ptr<TransientManager> m_transientManager;
    std::unique_ptr<MruWindowTracker> m_mruTracker;
    std::unique_ptr<DesksController> m_desks;
    std::unique_ptr<OverviewController> m_overview;
    std::vector<scene::Node *> m_deskOverlays;
    std::unique_ptr<ToplevelEventHandler> m_eventHandler;
    Placement m_placement;
    WindowModalityController m_modality;
    display::DisplayManager *m_displayManager = nullptr;
    QRect m_outputBounds;
    QRect m_workArea;
    std::vector<OutputArea> m_outputAreas;
    ExclusiveZones m_exclusiveZones;
    std::map<compositor::Surface *, ShellWindowMetadata> m_shellMetadata;
    std::map<const Window *, QString> m_windowOutputs;
    struct RememberedPlacement {
        QRect bounds;
        QRect output;
    };
    // By application id and restore id.
    std::map<std::pair<QString, QString>, RememberedPlacement> m_rememberedPlacements;

    std::map<compositor::Surface *, Window *> m_surfaceOwners;
    std::vector<compositor::Surface *> m_auxiliarySurfaces;
    // Outermost first.
    std::vector<ShellSurface *> m_popupGrabs;
    // Buttons whose press the window manager kept from clients: their release
    // is kept too, or a client would see a release without a press.
    std::set<uint32_t> m_consumedButtons;
    // Touch points that went down on a window's frame.
    std::map<int32_t, Window *> m_frameTouches;
    std::function<void(qint64 pid)> m_terminateProcess;
    std::function<void(qint64 pid)> m_forceQuitProcess;
    std::function<bool(Window *)> m_notRespondingPrompt;
    std::chrono::milliseconds m_quitGracePeriod {3000};
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_WINDOW_MANAGER_H_
