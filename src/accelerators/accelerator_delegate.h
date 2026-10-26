// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// What the accelerator commands need from outside the compositor process.
// Ash performs every action in-process through Shell::Get(); Cutefish's shell
// UI is a separate process reached over D-Bus, and this is the seam the tests
// replace.

#ifndef CUTEFISH_ACCELERATORS_ACCELERATOR_DELEGATE_H_
#define CUTEFISH_ACCELERATORS_ACCELERATOR_DELEGATE_H_

#include <functional>

namespace cutefish::accelerators {

class AcceleratorDelegate
{
public:
    virtual ~AcceleratorDelegate() = default;

    // org.cutefish.Launcher.toggle.
    virtual void toggleAppList() = 0;
    virtual void toggleSpotlight() = 0;

    // org.cutefish.Screenshot: every screen at once, a part of one chosen
    // with the pointer, the capture tools, and the end of a recording.
    virtual void takeScreenshot(bool toClipboard) = 0;
    virtual void takePartialScreenshot(bool toClipboard) = 0;
    virtual void startCaptureMode() = 0;
    virtual void stopScreenRecording() = 0;

    // org.cutefish.ClipboardHistory.toggle: shows or hides the picker.
    virtual void toggleClipboardHistory() = 0;

    // org.cutefish.Osd: changes the level and shows it.
    virtual void volumeDown() = 0;
    virtual void volumeUp() = 0;
    virtual void toggleMute() = 0;
    virtual void toggleMicrophoneMute() = 0;
    virtual void brightnessDown() = 0;
    virtual void brightnessUp() = 0;
    virtual void keyboardBrightnessDown() = 0;
    virtual void keyboardBrightnessUp() = 0;

    // Media playback (MPRIS).
    virtual void mediaPlayPause() = 0;
    virtual void mediaNextTrack() = 0;
    virtual void mediaPrevTrack() = 0;
    virtual void mediaStop() = 0;
};

// The production delegate.
class DBusAcceleratorDelegate : public AcceleratorDelegate
{
public:
    // Shell services callbacks wired in-process; unset, calls fall back to D-Bus.
    std::function<void()> launcherToggle;
    std::function<void()> spotlightToggle;
    std::function<void(bool)> screenshotTake;
    std::function<void(bool)> screenshotPartial;
    std::function<void()> screenshotCaptureMode;
    std::function<void()> screenshotStopRecording;
    std::function<void()> clipboardHistoryToggle;
    std::function<void()> osdVolumeDown;
    std::function<void()> osdVolumeUp;
    std::function<void()> osdToggleMute;
    std::function<void()> osdToggleMicrophoneMute;
    std::function<void()> osdBrightnessDown;
    std::function<void()> osdBrightnessUp;
    std::function<void()> osdKeyboardBrightnessDown;
    std::function<void()> osdKeyboardBrightnessUp;
    std::function<void()> mediaPlayPauseCallback;
    std::function<void()> mediaNextTrackCallback;
    std::function<void()> mediaPrevTrackCallback;
    std::function<void()> mediaStopCallback;

    void toggleAppList() override;
    void toggleSpotlight() override;
    void takeScreenshot(bool toClipboard) override;
    void takePartialScreenshot(bool toClipboard) override;
    void startCaptureMode() override;
    void stopScreenRecording() override;
    void toggleClipboardHistory() override;
    void volumeDown() override;
    void volumeUp() override;
    void toggleMute() override;
    void toggleMicrophoneMute() override;
    void brightnessDown() override;
    void brightnessUp() override;
    void keyboardBrightnessDown() override;
    void keyboardBrightnessUp() override;
    void mediaPlayPause() override;
    void mediaNextTrack() override;
    void mediaPrevTrack() override;
    void mediaStop() override;
};

} // namespace cutefish::accelerators

#endif // CUTEFISH_ACCELERATORS_ACCELERATOR_DELEGATE_H_
