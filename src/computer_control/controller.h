// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Lets an agent drive the primary seat once the user allows it: consent, the
// single current control, pausing on real input and protected targets. The
// pause follows remoting/host/remote_input_filter.cc at Chromium revision
// 8abbac30b9cf2fe976422c58c000c94685648b65.

#ifndef CUTEFISH_COMPUTER_CONTROL_CONTROLLER_H_
#define CUTEFISH_COMPUTER_CONTROL_CONTROLLER_H_

#include <QObject>
#include <QPointF>
#include <QString>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "input/types.h"
#include "input/xkb_tracker.h"

namespace cutefish::input {
class InputManager;
}

namespace cutefish::computer_control {

// The values are cutefish_computer_control_v1's.
enum class RevokeReason : uint32_t {
    Denied = 0,
    Stopped = 1,
    Locked = 2,
    Busy = 3,
    Unavailable = 4,
};

enum class Rejection : uint32_t {
    NotGranted = 0,
    Paused = 1,
    Protected = 2,
    Untypable = 3,
};

enum class ConsentResult {
    Allowed,
    Denied,
    Unavailable,
};

// The protocol side of one control.
class ControlClient
{
public:
    virtual ~ControlClient() = default;

    virtual void granted() = 0;
    virtual void revoked(RevokeReason reason) = 0;
    virtual void rejected(Rejection rejection) = 0;
};

// What the controller asks of the rest of the compositor.
struct Environment {
    // Asks the user whether `agentName` may take control. `answer` runs once,
    // later; the returned function withdraws the question before then.
    std::function<std::function<void()>(const QString &agentName,
                                        std::function<void(ConsentResult)> answer)>
            askConsent;
    // A window of a process that shows system dialogs is at `position`, or has
    // the keyboard focus.
    std::function<bool(const QPointF &position)> protectedAt;
    std::function<bool()> protectedFocus;
    // A text field of the focused client is enabled; an input method composes
    // the keys typed into it; commit sends text to it.
    std::function<bool()> textInputFocused;
    std::function<bool()> inputMethodGrabsKeyboard;
    std::function<bool(const std::string &text)> commitText;
    std::function<bool()> sessionBlocked;
    std::function<input::Timestamp()> now;
};

class Control;

class Controller : public QObject
{
    Q_OBJECT

public:
    // Real input blocks injection this long, as RemoteInputFilter's
    // kRemoteBlockTimeout.
    static constexpr std::chrono::milliseconds kPauseAfterInput {2000};

    Controller(input::InputManager *input, Environment environment, QObject *parent = nullptr);
    ~Controller() override;

    // A new control for `client`, which `owner` (its wl_client) holds. Only
    // one control is pending or granted at a time; a second one is revoked at
    // once with Busy. The control ends when the returned handle is destroyed.
    std::unique_ptr<Control> request(ControlClient *client, const void *owner,
                                     const QString &agentName);

    // A control is granted.
    bool isActive() const;
    QString agentName() const;

    // `owner` holds the granted control, so it may read the screen.
    bool allowsCapture(const void *owner) const;

    // Ends the pending or granted control with Stopped.
    void stop();

    // Ends it with Locked; call when the session becomes blocked.
    void sessionBlocked();

Q_SIGNALS:
    void activeChanged(bool active);

private:
    friend class Control;

    input::Timestamp now() const;
    bool paused(const Control *control) const;

    // Whether `control` may inject now, telling its client why not otherwise.
    // `protectedTarget` is asked last, once nothing else refuses.
    bool admits(Control *control, const std::function<bool()> &protectedTarget = {});
    void answer(Control *control, uint64_t serial, ConsentResult result);
    void end(Control *control, std::optional<RevokeReason> reason);
    void releaseAll(Control *control);
    void realInput();

    // Injects through the InputManager with real input detection off.
    void inject(const std::function<void()> &event);

    input::InputManager *m_input;
    Environment m_environment;
    std::set<Control *> m_controls;
    Control *m_current = nullptr;
    uint64_t m_serial = 0;
    bool m_injecting = false;
    std::optional<input::Timestamp> m_lastRealInput;
};

// One control, as cutefish_computer_control_v1 describes it. Input requests
// answer a refusal through ControlClient::rejected.
class Control
{
public:
    ~Control();

    Control(const Control &) = delete;
    Control &operator=(const Control &) = delete;

    void motion(const QPointF &position);
    void button(uint32_t button, bool pressed);
    void axis(int horizontal, int vertical);
    void key(uint32_t key, bool pressed);
    // The key that types `keysym` on the current layout, as `key`.
    void keysym(uint32_t keysym, bool pressed);
    void typeText(const std::string &text);

private:
    friend class Controller;

    enum class State {
        Pending,
        Granted,
        Revoked,
    };

    Control(Controller *controller, ControlClient *client, const void *owner,
            const QString &agentName);

    void stroke(const input::KeyStroke &stroke);

    Controller *m_controller;
    ControlClient *const m_client;
    const void *const m_owner;
    const QString m_agentName;
    State m_state = State::Pending;
    uint64_t m_serial = 0;
    std::function<void()> m_withdrawConsent;
    std::set<uint32_t> m_buttons;
    std::set<uint32_t> m_keys;
    // The key each keysym press pressed, for its release after a layout change.
    std::map<uint32_t, uint32_t> m_keysymKeys;
};

} // namespace cutefish::computer_control

#endif // CUTEFISH_COMPUTER_CONTROL_CONTROLLER_H_
