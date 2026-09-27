// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <list>

#include "render/gl_program.h"
#include "render/gl_render_pass.h"
#include "render/shape_mask.h"

namespace cutefish::render {

class BackdropFilter
{
public:
    explicit BackdropFilter(std::shared_ptr<EglContext> context)
        : m_context(std::move(context))
    {
    }

    bool initialize(const char *fragmentOverride = nullptr);
    void resetTextures();
    // Drops what no draw has used for a while; called with the context current.
    void frameEnded();
    void draw(Renderer &renderer, const QRect &lens, int radius, const QRect &clip,
              const GlassParams &glass, double scale, const QPainterPath &shape,
              const std::vector<BackdropSource> &sources, std::vector<RoundedShape> &clips,
              const std::function<void(const QRectF &, float *)> &quadFor,
              const std::function<void(const GlProgram &)> &applyRoundedClip);

private:
    // One per blur size, so a dock and a panel drawn in the same frame do not
    // reallocate each other's textures.
    struct Scratch {
        QSize coverage;
        QSize reduced;
        uint64_t used = 0;
        std::unique_ptr<Texture> capture;
        std::unique_ptr<Texture> opaqueCapture;
        std::unique_ptr<Texture> source;
        std::unique_ptr<Texture> temp;
        std::unique_ptr<Texture> result;
    };
    // `shape` is relative to its own bounds, so a moving popup keeps its mask.
    struct Mask {
        QPainterPath shape;
        ShapeRaster raster;
        uint64_t used = 0;
        std::unique_ptr<Texture> texture;
    };

    Scratch &scratch(const QSize &coverage, const QSize &reduced);
    Texture *mask(Renderer &renderer, const QPainterPath &shape, const ShapeRaster &raster);
    void applyGlassMaterial(const GlProgram &program, const GlassParams &glass,
                            const QSize &coverage, double scale);

    static constexpr size_t kCachedSizes = 4;
    static constexpr uint64_t kUnusedFrames = 60;
    std::shared_ptr<EglContext> m_context;
    GlProgram m_blurProgram;
    GlProgram m_blurMaskProgram;
    GlProgram m_glassProgram;
    uint64_t m_frame = 0;
    std::list<Scratch> m_scratch;
    std::list<Mask> m_masks;
};

} // namespace cutefish::render
