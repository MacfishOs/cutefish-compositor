// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_SCREENSHOT_SCREEN_GRABBER_H_
#define CUTEFISH_SHELL_SCREENSHOT_SCREEN_GRABBER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/bitmap.h"

namespace cutefish::wayland {
class ForeignToplevelList;
class ImageCaptureManager;
} // namespace cutefish::wayland

namespace cutefish::compositor {
class CaptureSource;
}

namespace cutefish::wm {
class Window;
}

namespace cutefish::shell {

// Still pictures of outputs or a window, upright, taken from the compositor's
// own capture path rather than through the capture protocol.
class ScreenGrabber
{
public:
    // One picture per output asked for, in that order, or one for a window;
    // a null bitmap where it failed.
    using Done = std::function<void(std::vector<ui::Bitmap> images)>;

    virtual ~ScreenGrabber() = default;

    virtual void grabOutputs(const std::vector<std::string> &outputs, bool cursor, Done done) = 0;
    // The window as it is shown: outline and shadow included, with the
    // transparent margins the shadow needs. The dock's body with its glass.
    virtual void grabWindow(wm::Window *window, bool cursor, Done done) = 0;
    // What `source` paints, as a window capture does.
    virtual void grabSource(std::shared_ptr<compositor::CaptureSource> source, bool cursor, Done done)
    {
        (void)source;
        (void)cursor;
        done({});
    }
};

// The session's: frames of the compositor's image capture, turned upright on
// a worker thread. A frame that fails is asked for again twice, 50 ms apart;
// whatever has not come after three seconds is given up.
std::unique_ptr<ScreenGrabber> createScreenGrabber(wayland::ImageCaptureManager *capture,
                                                   wayland::ForeignToplevelList *windows);

// The picture as the output shows it, from its buffer orientation and its
// wl_output transform.
ui::Bitmap uprightImage(const ui::Bitmap &image, uint32_t transform);

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SCREENSHOT_SCREEN_GRABBER_H_
