// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_BACKEND_INPUT_LIBINPUT_BACKEND_H_
#define CUTEFISH_BACKEND_INPUT_LIBINPUT_BACKEND_H_

#include "input/mouse_settings.h"
#include "input/seat_observer.h"
#include "input/touchpad_settings.h"
#include "input/touchscreen_settings.h"
#include "input/types.h"
#include <QObject>

#include <map>
#include <memory>
#include <set>

struct libinput;
struct libinput_event;
struct libinput_device;
struct udev;
class QSocketNotifier;

namespace cutefish::input {
class InputManager;
}

namespace cutefish::backend {

class Session;

// Opens the seat's input devices through libinput and pumps them from the Qt
// event loop, feeding an input::InputManager.
//
// Devices are opened through the Session, so the compositor never needs
// permission to /dev/input itself and the seat manager can revoke access across
// VT switches.
class LibinputBackend : public QObject, public input::SeatObserver
{
    Q_OBJECT

public:
    ~LibinputBackend() override;

    // Creates a udev-backed context assigned to the session's seat. Returns
    // nullptr if the context or the seat could not be acquired.
    static std::unique_ptr<LibinputBackend>
    create(Session *session, input::InputManager *inputManager, QObject *parent = nullptr);

    // Devices are suspended while another session owns the seat; libinput is
    // told so that it re-reads device state on resume.
    void setActive(bool active);

    void onSurfaceFocused(compositor::Surface *, compositor::Surface *) override { }
    // Every keyboard's lock lights follow the lock state the clients see.
    void onKeyboardModifierUpdated() override;
    void onKeyboardLayoutUpdated() override { onKeyboardModifierUpdated(); }

private:
    LibinputBackend(Session *session, input::InputManager *inputManager, QObject *parent);

    bool open();
    void dispatch();
    void processEvent(libinput_event *event);
    // Sorts a new device into the mice or the touchpads, by udev's
    // classification, and applies their preferences.
    void addPointingDevice(libinput_device *device);
    void removePointingDevice(libinput_device *device);
    void applyMouseSettings(libinput_device *device);
    void applyTouchpadSettings(libinput_device *device);
    void addTouchscreen(libinput_device *device);
    void removeTouchscreen(libinput_device *device);
    void updateTouchscreenList();
    void applyTouchscreenSettings(libinput_device *device);

    static int openRestricted(const char *path, int flags, void *userData);
    static void closeRestricted(int fd, void *userData);

    Session *m_session = nullptr;
    input::InputManager *m_inputManager = nullptr;

    udev *m_udev = nullptr;
    libinput *m_libinput = nullptr;
    QSocketNotifier *m_notifier = nullptr;

    // Session device handles, keyed by the fd libinput knows them as.
    std::map<int, int> m_deviceIds;
    bool m_active = true;
    input::MouseSettings m_mouseSettings;
    input::TouchpadSettings m_touchpadSettings;
    input::TouchscreenSettings m_touchscreenSettings;
    std::set<libinput_device *> m_mice;
    std::set<libinput_device *> m_touchpads;
    std::set<libinput_device *> m_touchscreens;
    std::map<libinput_device *, input::KeyboardModifierLayout> m_keyboardLayouts;
    uint32_t m_leds = 0;
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_INPUT_LIBINPUT_BACKEND_H_
