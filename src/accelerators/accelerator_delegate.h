// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// What the accelerator commands need from outside the compositor process.
// Ash performs every action in-process through Shell::Get(); Cutefish's shell
// UI is a separate process reached over D-Bus, and this is the seam the tests
// replace.

#ifndef CUTEFISH_ACCELERATORS_ACCELERATOR_DELEGATE_H_
#define CUTEFISH_ACCELERATORS_ACCELERATOR_DELEGATE_H_

namespace cutefish::accelerators {

class AcceleratorDelegate
{
public:
    virtual ~AcceleratorDelegate() = default;

    // com.cutefish.Launcher.toggle.
    virtual void toggleAppList() = 0;

    // com.cutefish.Screenshot: every screen at once, a part of one chosen
    // with the pointer, the capture tools, and the end of a recording.
    virtual void takeScreenshot(bool toClipboard) = 0;
    virtual void takePartialScreenshot(bool toClipboard) = 0;
    virtual void startCaptureMode() = 0;
    virtual void stopScreenRecording() = 0;
};

// The production delegate.
class DBusAcceleratorDelegate : public AcceleratorDelegate
{
public:
    void toggleAppList() override;
    void takeScreenshot(bool toClipboard) override;
    void takePartialScreenshot(bool toClipboard) override;
    void startCaptureMode() override;
    void stopScreenRecording() override;
};

} // namespace cutefish::accelerators

#endif // CUTEFISH_ACCELERATORS_ACCELERATOR_DELEGATE_H_
