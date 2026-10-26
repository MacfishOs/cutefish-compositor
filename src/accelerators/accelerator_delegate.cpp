// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "accelerators/accelerator_delegate.h"

#include "base/dbus.h"

namespace cutefish::accelerators {

namespace {

// Calls without waiting for the answer.
template<typename... Args>
void call(const char *service, const char *path, const char *method, const Args &...arguments)
{
    if (base::dbus::Connection *bus = base::dbus::Connection::session())
        base::dbus::Proxy(bus, service, path, service).send(method, arguments...);
}

template<typename... Args>
void callScreenshot(const char *method, const Args &...arguments)
{
    call("org.cutefish.Screenshot", "/Screenshot", method, arguments...);
}

void callOsd(const char *method)
{
    call("org.cutefish.Osd", "/Osd", method);
}

} // namespace

void DBusAcceleratorDelegate::toggleAppList()
{
    if (launcherToggle) {
        launcherToggle();
        return;
    }
    call("org.cutefish.Launcher", "/Launcher", "toggle");
}

void DBusAcceleratorDelegate::toggleSpotlight()
{
    if (spotlightToggle) {
        spotlightToggle();
        return;
    }
    call("org.cutefish.Spotlight", "/Spotlight", "toggle");
}

void DBusAcceleratorDelegate::takeScreenshot(bool toClipboard)
{
    if (screenshotTake) {
        screenshotTake(toClipboard);
        return;
    }
    callScreenshot("takeScreenshot", toClipboard);
}

void DBusAcceleratorDelegate::takePartialScreenshot(bool toClipboard)
{
    if (screenshotPartial) {
        screenshotPartial(toClipboard);
        return;
    }
    callScreenshot("takePartialScreenshot", toClipboard);
}

void DBusAcceleratorDelegate::startCaptureMode()
{
    if (screenshotCaptureMode) {
        screenshotCaptureMode();
        return;
    }
    callScreenshot("startCaptureMode");
}

void DBusAcceleratorDelegate::stopScreenRecording()
{
    if (screenshotStopRecording) {
        screenshotStopRecording();
        return;
    }
    callScreenshot("stopRecording");
}

void DBusAcceleratorDelegate::toggleClipboardHistory()
{
    if (clipboardHistoryToggle) {
        clipboardHistoryToggle();
        return;
    }
    call("org.cutefish.ClipboardHistory", "/ClipboardHistory", "toggle");
}

void DBusAcceleratorDelegate::volumeDown()
{
    if (osdVolumeDown) {
        osdVolumeDown();
        return;
    }
    callOsd("volumeDown");
}

void DBusAcceleratorDelegate::volumeUp()
{
    if (osdVolumeUp) {
        osdVolumeUp();
        return;
    }
    callOsd("volumeUp");
}

void DBusAcceleratorDelegate::toggleMute()
{
    if (osdToggleMute) {
        osdToggleMute();
        return;
    }
    callOsd("toggleMute");
}

void DBusAcceleratorDelegate::toggleMicrophoneMute()
{
    if (osdToggleMicrophoneMute) {
        osdToggleMicrophoneMute();
        return;
    }
    callOsd("toggleMicrophoneMute");
}

void DBusAcceleratorDelegate::brightnessDown()
{
    if (osdBrightnessDown) {
        osdBrightnessDown();
        return;
    }
    callOsd("brightnessDown");
}

void DBusAcceleratorDelegate::brightnessUp()
{
    if (osdBrightnessUp) {
        osdBrightnessUp();
        return;
    }
    callOsd("brightnessUp");
}

void DBusAcceleratorDelegate::keyboardBrightnessDown()
{
    if (osdKeyboardBrightnessDown) {
        osdKeyboardBrightnessDown();
        return;
    }
    callOsd("keyboardBrightnessDown");
}

void DBusAcceleratorDelegate::keyboardBrightnessUp()
{
    if (osdKeyboardBrightnessUp) {
        osdKeyboardBrightnessUp();
        return;
    }
    callOsd("keyboardBrightnessUp");
}

void DBusAcceleratorDelegate::mediaPlayPause()
{
    if (mediaPlayPauseCallback)
        mediaPlayPauseCallback();
}

void DBusAcceleratorDelegate::mediaNextTrack()
{
    if (mediaNextTrackCallback)
        mediaNextTrackCallback();
}

void DBusAcceleratorDelegate::mediaPrevTrack()
{
    if (mediaPrevTrackCallback)
        mediaPrevTrackCallback();
}

void DBusAcceleratorDelegate::mediaStop()
{
    if (mediaStopCallback)
        mediaStopCallback();
}

} // namespace cutefish::accelerators
