// Copyright 2019 The Chromium Authors
// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/wm/core/shadow_controller_delegate.h
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#ifndef CUTEFISH_WM_SHADOW_CONTROLLER_DELEGATE_H_
#define CUTEFISH_WM_SHADOW_CONTROLLER_DELEGATE_H_

#include "wm/shadow_types.h"

namespace cutefish::wm {

class Window;

// The policy half of ShadowController. Upstream carries only the veto
// (ShouldShowShadowForWindow) because the elevation lives in a window property
// that ash writes; Cutefish has no property bag, so the delegate answers both
// questions.
class ShadowControllerDelegate
{
public:
    virtual ~ShadowControllerDelegate() = default;

    // False means the compositor draws no shadow at all for this window.
    virtual bool shouldShowShadowForWindow(const Window* window) = 0;

    // Only asked when shouldShowShadowForWindow said yes. Returning None is
    // allowed and means the same thing.
    virtual ShadowElevation elevationForWindow(const Window* window) = 0;
};

} // namespace cutefish::wm

#endif // CUTEFISH_WM_SHADOW_CONTROLLER_DELEGATE_H_
