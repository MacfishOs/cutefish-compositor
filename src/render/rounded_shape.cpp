// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/rounded_shape.h"

namespace cutefish::render {

namespace {
bool s_continuousCorners = true;
}

bool continuousCorners()
{
    return s_continuousCorners;
}

void setContinuousCorners(bool continuous)
{
    s_continuousCorners = continuous;
}

} // namespace cutefish::render
