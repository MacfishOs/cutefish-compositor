// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_WM_OVERVIEW_OVERVIEW_CONTROLLER_H_
#define CUTEFISH_WM_OVERVIEW_OVERVIEW_CONTROLLER_H_

#include <optional>
#include <set>
#include <vector>

#include <QMetaObject>
#include <QPointF>
#include <QRectF>

#include "input/event_sink.h"
#include "scene/overview_node.h"
#include "wm/activation.h"
#include "wm/desks/desks_controller.h"

namespace cutefish::wm {

class Desk;
class Window;
class WindowManager;

// The window overview: every window of the active desk spread out where it
// can be seen whole, with the desk bar along the top. Three fingers up on the
// touchpad bring it in following the fingers, three down take it away - to
// the window under the pointer, when there is one - as does the shortcut,
// Escape, a click on a window or on empty space.
//
// Everything on screen is a function of one progress value and a layout
// computed when the overview starts, drawn by an OverviewNode and an
// OverviewDeskBarNode. While it is shown, the pointer and the keyboard are
// the overview's, except over the panels and the dock, which stay usable.
// Upstream Ash (ash/wm/overview) fills its rows by recency; here the rows keep
// the windows roughly where they are, each as large as the room around it
// allows (overview_layout.h).
class OverviewController : public input::EventSink,
                           public ActivationObserver,
                           public DesksObserver
{
public:
    enum class State {
        Hidden,
        // Heading in, with the fingers or on its own.
        Entering,
        Shown,
        // Heading out; input goes to the windows again.
        Exiting,
    };

    // Between the windows, and between them and the edges of the work area.
    static constexpr int kWindowGap = 32;
    static constexpr int kMargin = 24;
    // The pointer moves this far with the button down before a window drags.
    static constexpr double kDragThreshold = 8;

    // The desk bar: previews with the names under them.
    static constexpr int kBarPadding = 12;
    static constexpr int kBarSpacing = 12;
    static constexpr int kLabelHeight = 24;
    // On the bar a dragged window's shorter side is no less than this much of
    // the bar's height (Ash's kScaleFactorForMinimumSideLength).
    static constexpr double kMinimumSideOnBar = 0.5;

    explicit OverviewController(WindowManager *manager);
    OverviewController(const OverviewController &) = delete;
    OverviewController &operator=(const OverviewController &) = delete;
    ~OverviewController() override;

    // Where the overview draws. Without them it never shows.
    void setNodes(scene::OverviewNode *node, scene::OverviewDeskBarNode *bar);
    scene::OverviewNode *node() const { return m_node; }
    scene::OverviewDeskBarNode *deskBar() const { return m_bar; }

    // Receives whatever the overview does not take.
    void setNext(input::EventSink *next) { m_next = next; }

    State state() const { return m_state; }
    bool isActive() const { return m_state != State::Hidden; }
    // Coming in or shown: the pointer and the keys are the overview's, and
    // there is no active window for the shortcuts to act on.
    bool capturesInput() const;
    // Moving between shown and hidden, rather than standing at either.
    bool isTransitioning() const
    {
        return m_state == State::Entering || m_state == State::Exiting;
    }

    // The shortcut.
    void toggle();
    bool enter();
    // Leaves, activating `window` first when given; it lands last, on top.
    void exit(Window *window = nullptr);
    // Gone at once, with no animation.
    void exitImmediately();
    // Lands a transition at once where it was heading: all the way in, or
    // gone. Not while the fingers hold it.
    void completeTransition();

    // Three fingers up (`upward`) or down. Deltas are in progress, 1 being
    // the whole way in; times in milliseconds on the steady clock.
    bool startSwipe(bool upward);
    void updateSwipe(double delta, double timeMs);
    void endSwipe(double timeMs);

    struct Hit {
        enum class Type {
            None,
            Item,
            // A desk in the desk bar, its close button, its new-desk button,
            // or the rest of it.
            Desk,
            DeskClose,
            NewDesk,
            Bar,
            // A panel or the dock, which keep their input.
            Panel,
        };
        Type type = Type::None;
        int index = -1;
        int bar = -1;
    };
    Hit hitTest(const QPointF &location) const;

    // The windows shown, in drawing order, and where each goes.
    struct Item {
        Window *window = nullptr;
        QRectF bounds;
    };
    const std::vector<Item> &items() const { return m_items; }
    const std::vector<scene::OverviewDeskBarNode::Bar> &bars() const { return m_bars; }
    int hovered() const { return m_hovered; }
    int selected() const { return m_selected; }

    // --- input::EventSink ---
    bool onPointerMotion(input::Timestamp timestamp, const QPointF &location) override;
    bool onPointerButton(input::Timestamp timestamp, const QPointF &location, uint32_t evdevButton,
                         bool pressed) override;
    bool onPointerAxis(input::Timestamp timestamp, const QPointF &location, const QPointF &offset,
                       bool discrete) override;
    bool onPointerAxisStop(input::Timestamp timestamp, const QPointF &location) override;
    bool onKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed) override;
    bool onSecureKey(input::Timestamp timestamp, uint32_t evdevCode, bool pressed) override;
    bool onTouchDown(input::Timestamp timestamp, int32_t id, const QPointF &location) override;
    bool onTouchMotion(input::Timestamp timestamp, int32_t id, const QPointF &location) override;
    bool onTouchUp(input::Timestamp timestamp, int32_t id) override;
    void onTouchCancel(input::Timestamp timestamp) override;

    // --- ActivationObserver ---
    void onWindowActivated(Window *gainedActive, Window *lostActive) override;

    // --- DesksObserver ---
    void onDeskAdded(const Desk *desk) override;
    void onDeskRemoved(const Desk *desk) override;
    void onDeskActivationChanged(const Desk *activated, const Desk *deactivated) override;
    void onDeskNameChanged(const Desk *desk) override;
    void onWindowDeskChanged(Window *window) override;

private:
    struct Press {
        Hit hit;
        QPointF location;
        // The touch point pressing, or none for the pointer.
        std::optional<int32_t> touch;
    };

    // Whether `window` is one of the overview's items.
    bool isOverviewWindow(const Window *window) const;
    // Lays the windows out again; `animate` moves the ones already shown to
    // their new places on a tween.
    void rebuild(bool animate);
    void buildBars();
    // Where the dragged item is drawn with the pointer at `location`: shrunk
    // on its way to the desk bar, as Ash's OverviewWindowDragController does,
    // and held where it was grabbed.
    QRectF draggedBounds(const QPointF &location) const;
    void updateNodes(bool animate);
    void updateChrome();
    QString deskName(int index) const;
    // Whether the desks in the bar can be removed with their close buttons.
    bool canCloseDesks() const;

    void takePointer();
    void releasePointer();

    void landDeskSwitch();
    // Clicks, touches and Enter act while it is shown or coming in on its
    // own, not while the fingers move it.
    bool acceptsPresses() const;
    // Activates `window` from the overview, which does not end it; it is
    // raised, so it lands last, on top.
    void activateForExit(Window *window);
    void begin();
    void finish();
    void onNodeFinished(double target);

    void press(const Hit &hit, const QPointF &location, std::optional<int32_t> touch);
    void move(const QPointF &location);
    void release(const QPointF &location);
    void cancelPress();
    void drop(const QPointF &location);
    void activateItem(int index);
    void moveSelection(int dx, int dy);
    void setHover(const Hit &hit);

    WindowManager *m_manager = nullptr;
    scene::OverviewNode *m_node = nullptr;
    scene::OverviewDeskBarNode *m_bar = nullptr;
    input::EventSink *m_next = nullptr;
    State m_state = State::Hidden;
    std::vector<Item> m_items;
    std::vector<scene::OverviewDeskBarNode::Bar> m_bars;
    // For each bar's desks, the desk each one is.
    std::vector<std::vector<Desk *>> m_barDesks;

    int m_hovered = -1;
    // The window three fingers down take the overview away to.
    Window *m_swipeTarget = nullptr;
    int m_selected = -1;
    Hit m_barHover;
    std::optional<Press> m_press;
    int m_dragged = -1;
    QPointF m_dragOffset;
    QPointF m_pointer;
    bool m_pointerTaken = false;
    // The cursor is the default one from the overview's first frame to its
    // last, whatever the windows under it ask for.
    bool m_cursorOverridden = false;
    // Activation the overview does itself, which does not end it.
    bool m_activating = false;
    // Buttons, keys and touch points the overview took, whose release it
    // takes too.
    std::set<uint32_t> m_buttons;
    std::set<uint32_t> m_keys;
    std::set<int32_t> m_touches;
    QPointF m_touchLocation;

    QMetaObject::Connection m_addedConnection;
    QMetaObject::Connection m_removedConnection;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_OVERVIEW_OVERVIEW_CONTROLLER_H_
