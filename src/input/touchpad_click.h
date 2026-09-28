// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_INPUT_TOUCHPAD_CLICK_H_
#define CUTEFISH_INPUT_TOUCHPAD_CLICK_H_

#include "input/types.h"

#include <QPointF>
#include <QSizeF>

namespace cutefish::input {

// A finger on a touchpad, in millimetres from the pad's top-left corner.
struct TouchpadContact {
    QPointF origin;
    QPointF position;
    Timestamp downTime;
};

// Whether a physical press with these two fingers on the pad is a two-finger
// click. libinput's clickfinger takes two fingers more than 30 mm apart
// vertically, or 20 mm with one in the bottom 20 mm, for a thumb and a finger;
// an index and middle finger laid diagonally on a 60 mm pad often are. This
// judges them as FingerButtonClick::EvaluateTwoFingerButtonType in ChromiumOS
// libgestures (src/immediate_interpreter.cc) does, without its pressure
// checks: fingers within an ellipse 50 mm wide and 45 mm tall are together,
// unless one rested in the bottom 10 mm before the other came down.
bool isTwoFingerClick(const TouchpadContact &first, const TouchpadContact &second,
                      Timestamp pressTime, const QSizeF &padSize);

} // namespace cutefish::input

#endif // CUTEFISH_INPUT_TOUCHPAD_CLICK_H_
