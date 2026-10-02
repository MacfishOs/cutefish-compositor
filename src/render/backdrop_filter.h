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
    // Whether the blur of surface `owner` kept its result in the target `scope` names.
    bool keeps(uint64_t scope, uint64_t owner) const;
    // Starts a pass into that target: the blurs of `fresh` read what they did
    // in the last one and draw what they kept; the others' results are dropped.
    void beginPass(uint64_t scope, const std::vector<uint64_t> &fresh);
    // `lens` is the exact device rectangle; it is rounded outward to pixels here.
    // With a `scope`, the blurred backdrop is kept for the next pass, by `owner`.
    void draw(Renderer &renderer, const QRectF &lens, int radius, const QRect &clip,
              const GlassParams &glass, double scale, const QPainterPath &shape,
              const std::vector<BackdropSource> &sources, std::vector<RoundedShape> &clips,
              const std::function<void(const QRectF &, float *)> &quadFor,
              const std::function<void(const GlProgram &)> &applyRoundedClip,
              uint64_t scope = 0, uint64_t owner = 0);

private:
    // Kept per size, so the dock, the panel and the windows drawn in one
    // frame do not reallocate each other's textures; plain blurs share them.
    struct Scratch {
        // Empty for a plain blur, whose textures serve any coverage.
        QSize coverage;
        QSize size;
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

    // A blurred backdrop, drawn again while what lies behind it stays the same.
    struct Kept {
        uint64_t scope = 0;
        uint64_t owner = 0;
        QRect coverage;
        QSize reduced;
        QSize allocated;
        float sigma = 0;
        // A surface drawn twice in one pass keeps only its first blur.
        bool drawn = false;
        uint64_t used = 0;
        std::unique_ptr<Texture> result;
    };

    Scratch &scratch(const QSize &coverage, const QSize &size);
    // Copies `coverage` of the target into `textures.source`, reduced to `reduced`.
    bool captureBackdrop(Renderer &renderer, Scratch &textures,
                         const std::vector<BackdropSource> &sources, const QRect &coverage,
                         const QSize &reduced, const QRect &allocatedBounds);
    // Copies `coverage` of a lone upright source into the top left of the
    // bound pass at `reduced` in one blit. False when it cannot, and nothing is drawn.
    bool blitReduced(const std::vector<BackdropSource> &sources, const QRect &coverage,
                     const QSize &reduced, unsigned int destination);
    Texture *mask(Renderer &renderer, const QPainterPath &shape, const ShapeRaster &raster);
    void applyGlassMaterial(const GlProgram &program, const GlassParams &glass,
                            const QSize &coverage, double scale);

    static constexpr size_t kCachedSizes = 8;
    static constexpr int kScratchStep = 64;
    static constexpr uint64_t kUnusedFrames = 60;
    std::shared_ptr<EglContext> m_context;
    GlProgram m_blurProgram;
    GlProgram m_blurMaskProgram;
    GlProgram m_glassProgram;
    uint64_t m_frame = 0;
    std::list<Scratch> m_scratch;
    std::list<Mask> m_masks;
    std::list<Kept> m_kept;
};

} // namespace cutefish::render
