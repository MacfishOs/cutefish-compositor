// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "render/gl_program.h"

namespace cutefish::render {

// The farthest texel blur_fragment_shader reaches: fifteen linear taps of two.
inline constexpr int kGaussianMaxReach = 30;

// Sets blur_fragment_shader's kernel to a Gaussian of `sigma` texels, cut off
// `reach` texels out; `stepX` and `stepY` are one texel in texture
// coordinates. A sigma of zero copies. `program` must be in use.
void setGaussianKernel(const GlProgram &program, float sigma, float stepX, float stepY,
                       int reach = kGaussianMaxReach);

} // namespace cutefish::render
