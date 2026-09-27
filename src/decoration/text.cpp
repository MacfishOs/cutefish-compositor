// Copyright 2012 The Chromium Authors
// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/gfx/render_text{,_harfbuzz}.cc
//           chromium/src/ui/gfx/font_fallback_linux.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a

#include "decoration/text.h"

#include <algorithm>
#include <cmath>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include <limits>
#include <memory>
#include <vector>

#include <QImage>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/utf16.h>

namespace cutefish::decoration {
namespace {

constexpr int kMaxTitleLength = 4096;

QString singleLine(const QString& text)
{
    QString result = text.left(kMaxTitleLength);
    if (!result.isEmpty() && result.back().isHighSurrogate())
        result.chop(1);
    for (QChar& ch : result) {
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QLatin1Char('\t') ||
            ch == QChar(0x2028) || ch == QChar(0x2029))
            ch = QLatin1Char(' ');
        else if (ch.isNull())
            ch = QChar::ReplacementCharacter;
    }
    return result;
}

std::vector<int> boundaries(const QString& text)
{
    UErrorCode error = U_ZERO_ERROR;
    std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> iterator(
        ubrk_open(UBRK_CHARACTER, nullptr, reinterpret_cast<const UChar*>(text.utf16()),
                  text.size(), &error),
        ubrk_close);
    std::vector<int> result {0};
    if (U_FAILURE(error))
        return result;
    for (int end = ubrk_next(iterator.get()); end != UBRK_DONE; end = ubrk_next(iterator.get()))
        result.push_back(end);
    return result;
}

struct Font {
    FT_Face face = nullptr;
    hb_font_t* hb = nullptr;
    float scale = 1;

    ~Font()
    {
        if (hb)
            hb_font_destroy(hb);
        if (face)
            FT_Done_Face(face);
    }
};

// Used only on the compositor thread. Faces stay alive through layout and painting.
class Fonts
{
public:
    Fonts() { FT_Init_FreeType(&m_library); }

    ~Fonts()
    {
        m_fonts.clear();
        if (m_candidates)
            FcFontSetDestroy(m_candidates);
        if (m_library)
            FT_Done_FreeType(m_library);
    }

    bool setSize(int size)
    {
        if (!m_library || size <= 0 || size > 128)
            return false;
        if (size == m_size && m_candidates)
            return true;
        // Keep configuration ownership local; other libraries may use Fontconfig too.
        if (!m_config)
            m_config.reset(FcInitLoadConfigAndFonts());
        if (!m_config)
            return false;
        m_fonts.clear();
        if (m_candidates)
            FcFontSetDestroy(m_candidates);
        m_candidates = nullptr;
        m_size = size;
        FcPattern* pattern = FcPatternCreate();
        if (!pattern)
            return false;
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif"));
        FcPatternAddDouble(pattern, FC_PIXEL_SIZE, size);
        FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
        FcConfigSubstitute(m_config.get(), pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        m_candidates = FcFontSort(m_config.get(), pattern, FcFalse, nullptr, &result);
        FcPatternDestroy(pattern);
        if (!m_candidates)
            return false;
        m_fonts.resize(m_candidates->nfont);
        return true;
    }

    Font* forText(const QString& text, int start, int end)
    {
        bool preferColor = false;
        bool textPresentation = false;
        bool hasVisibleCharacter = false;
        for (int offset = start; offset < end;) {
            UChar32 code;
            U16_NEXT(text.utf16(), offset, end, code);
            preferColor |= code == 0xFE0F || u_hasBinaryProperty(code, UCHAR_EMOJI_PRESENTATION);
            textPresentation |= code == 0xFE0E;
            hasVisibleCharacter |= !u_hasBinaryProperty(code, UCHAR_DEFAULT_IGNORABLE_CODE_POINT);
        }
        // Formatting-only clusters remain in bidi/shaping context, but must not
        // introduce a fallback font's line metrics without drawing any text.
        if (!hasVisibleCharacter)
            return nullptr;
        preferColor &= !textPresentation;
        bool bestColor = false;
        int bestMissing = std::numeric_limits<int>::max();
        Font* best = nullptr;
        for (int i = 0; i < m_candidates->nfont; ++i) {
            FcCharSet* charset = nullptr;
            if (FcPatternGetCharSet(m_candidates->fonts[i], FC_CHARSET, 0, &charset) !=
                FcResultMatch)
                continue;
            int missing = 0;
            for (int offset = start; offset < end;) {
                UChar32 code;
                U16_NEXT(text.utf16(), offset, end, code);
                if (!u_hasBinaryProperty(code, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) &&
                    !FcCharSetHasChar(charset, code))
                    ++missing;
            }
            FcBool colored = FcFalse;
            FcPatternGetBool(m_candidates->fonts[i], FC_COLOR, 0, &colored);
            if (missing > bestMissing ||
                (missing == bestMissing && !(preferColor && colored && !bestColor)))
                continue;
            Font* font = load(i);
            if (!font)
                continue;
            best = font;
            bestMissing = missing;
            bestColor = colored;
            if (!missing && (!preferColor || colored))
                break;
        }
        return best;
    }

private:
    Font* load(int index)
    {
        if (m_fonts[index])
            return m_fonts[index].get();
        FcChar8* file = nullptr;
        int faceIndex = 0;
        FcPattern* pattern = m_candidates->fonts[index];
        if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch)
            return nullptr;
        FcPatternGetInteger(pattern, FC_INDEX, 0, &faceIndex);
        auto font = std::make_unique<Font>();
        if (FT_New_Face(m_library, reinterpret_cast<const char*>(file), faceIndex, &font->face))
            return nullptr;
        if (FT_Set_Pixel_Sizes(font->face, 0, m_size)) {
            if (!font->face->num_fixed_sizes)
                return nullptr;
            int strike = 0;
            for (int i = 1; i < font->face->num_fixed_sizes; ++i) {
                if (std::abs(font->face->available_sizes[i].y_ppem / 64.0 - m_size) <
                    std::abs(font->face->available_sizes[strike].y_ppem / 64.0 - m_size))
                    strike = i;
            }
            if (FT_Select_Size(font->face, strike))
                return nullptr;
            font->scale = m_size / static_cast<float>(font->face->size->metrics.y_ppem);
        }
        font->hb = hb_ft_font_create_referenced(font->face);
        hb_ft_font_set_load_flags(font->hb, FT_LOAD_DEFAULT);
        m_fonts[index] = std::move(font);
        return m_fonts[index].get();
    }

    FT_Library m_library = nullptr;
    std::unique_ptr<FcConfig, decltype(&FcConfigDestroy)> m_config {nullptr, FcConfigDestroy};
    FcFontSet* m_candidates = nullptr;
    std::vector<std::unique_ptr<Font>> m_fonts;
    int m_size = 0;
};

struct Glyph {
    Font* font;
    uint32_t id;
    float x;
    float y;
};

struct Layout {
    std::vector<Glyph> glyphs;
    float advance = 0;
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;

    QSize size() const { return QSize(right - left, bottom - top); }
};

struct Run {
    int start;
    int end;
    UScriptCode script;
    Font* font;
};

void shapeRun(const QString& text, const Run& run, bool rtl, Layout& layout)
{
    if (!run.font)
        return;
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf16(buffer, text.utf16(), text.size(), run.start, run.end - run.start);
    hb_buffer_set_script(buffer, hb_script_from_string(uscript_getShortName(run.script), -1));
    hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_language(buffer, hb_language_get_default());
    hb_shape(run.font->hb, buffer, nullptr, 0);
    unsigned count = 0;
    const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
    const float unit = run.font->scale / 64.0f;
    const auto& metrics = run.font->face->size->metrics;
    layout.top = std::min(layout.top, -static_cast<int>(std::ceil(metrics.ascender * unit)));
    layout.bottom =
        std::max(layout.bottom, -static_cast<int>(std::floor(metrics.descender * unit)));
    for (unsigned i = 0; i < count; ++i) {
        Glyph glyph {run.font, infos[i].codepoint, layout.advance + positions[i].x_offset * unit,
                     -positions[i].y_offset * unit};
        layout.glyphs.push_back(glyph);
        if (!FT_Load_Glyph(run.font->face, glyph.id, FT_LOAD_DEFAULT)) {
            const auto& gm = run.font->face->glyph->metrics;
            layout.left = std::min(layout.left,
                                   static_cast<int>(std::floor(glyph.x + gm.horiBearingX * unit)));
            layout.right = std::max(
                layout.right,
                static_cast<int>(std::ceil(glyph.x + (gm.horiBearingX + gm.width) * unit)));
            layout.top = std::min(layout.top,
                                  static_cast<int>(std::floor(glyph.y - gm.horiBearingY * unit)));
            layout.bottom = std::max(
                layout.bottom,
                static_cast<int>(std::ceil(glyph.y + (gm.height - gm.horiBearingY) * unit)));
        }
        layout.advance += positions[i].x_advance * unit;
    }
    layout.right = std::max(layout.right, static_cast<int>(std::ceil(layout.advance)));
    hb_buffer_destroy(buffer);
}

Layout shape(const QString& text, int pixelSize, Fonts& fonts)
{
    Layout layout;
    if (text.isEmpty() || !fonts.setSize(pixelSize))
        return layout;
    UErrorCode error = U_ZERO_ERROR;
    std::unique_ptr<UBiDi, decltype(&ubidi_close)> bidi(ubidi_open(), ubidi_close);
    ubidi_setPara(bidi.get(), reinterpret_cast<const UChar*>(text.utf16()), text.size(),
                  UBIDI_DEFAULT_LTR, nullptr, &error);
    const int count = ubidi_countRuns(bidi.get(), &error);
    if (U_FAILURE(error))
        return layout;
    const auto breaks = boundaries(text);
    for (int visual = 0; visual < count; ++visual) {
        int start = 0, length = 0;
        const bool rtl = ubidi_getVisualRun(bidi.get(), visual, &start, &length) == UBIDI_RTL;
        std::vector<Run> runs;
        UScriptCode previousScript = USCRIPT_COMMON;
        for (int offset = start; offset < start + length;) {
            auto next = std::upper_bound(breaks.begin(), breaks.end(), offset);
            int end = next == breaks.end() ? start + length : std::min(*next, start + length);
            UScriptCode script = USCRIPT_COMMON;
            for (int pos = offset; pos < end;) {
                UChar32 code;
                U16_NEXT(text.utf16(), pos, end, code);
                error = U_ZERO_ERROR;
                auto candidate = uscript_getScript(code, &error);
                if (U_SUCCESS(error) && candidate != USCRIPT_COMMON &&
                    candidate != USCRIPT_INHERITED) {
                    script = candidate;
                    break;
                }
            }
            if (script == USCRIPT_COMMON)
                script = previousScript;
            Font* font = fonts.forText(text, offset, end);
            if (!runs.empty() && runs.back().font == font &&
                (runs.back().script == script || runs.back().script == USCRIPT_COMMON)) {
                runs.back().end = end;
                runs.back().script = script;
            } else {
                runs.push_back({offset, end, script, font});
            }
            previousScript = script;
            offset = end;
        }
        if (rtl)
            std::reverse(runs.begin(), runs.end());
        for (const auto& run : runs)
            shapeRun(text, run, rtl, layout);
    }
    return layout;
}

QString withEllipsis(const QString& prefix, UBiDiDirection direction)
{
    QString result = prefix + QChar(0x2026);
    for (int i = prefix.size(); i > 0;) {
        UChar32 code;
        U16_PREV(prefix.utf16(), 0, i, code);
        const auto type = u_charDirection(code);
        if (type == U_LEFT_TO_RIGHT || type == U_RIGHT_TO_LEFT || type == U_RIGHT_TO_LEFT_ARABIC) {
            const bool rtl = type != U_LEFT_TO_RIGHT;
            if (rtl != (direction == UBIDI_RTL))
                result += QChar(rtl ? 0x200F : 0x200E);
            break;
        }
    }
    return result;
}

} // namespace

struct TextRenderer::Impl {
    Fonts fonts;
};

TextRenderer::TextRenderer()
    : m_impl(std::make_unique<Impl>())
{
}

TextRenderer::~TextRenderer() = default;

QSize TextRenderer::textSize(const QString& text, int pixelSize)
{
    return shape(singleLine(text), pixelSize, m_impl->fonts).size();
}

Image TextRenderer::renderText(const QString& text, int pixelSize, const render::Color& color,
                               double rasterDensity)
{
    const Layout layout = shape(singleLine(text), pixelSize, m_impl->fonts);
    rasterDensity = std::clamp(rasterDensity, 1.0, 4.0);
    Image image(QSize(int(std::ceil(layout.size().width() * rasterDensity)),
                      int(std::ceil(layout.size().height() * rasterDensity))));
    for (const auto& glyph : layout.glyphs) {
        FT_Face face = glyph.font->face;

        struct RestoreSize {
            FT_Face face;
            int size;

            ~RestoreSize()
            {
                if (face)
                    FT_Set_Pixel_Sizes(face, 0, size);
            }
        } restore {nullptr, pixelSize};

        const bool scalable = FT_IS_SCALABLE(face);
        if (rasterDensity > 1 && scalable) {
            // In 26.6 fixed point, so a fractional density hints the glyphs at
            // the size they end up on screen.
            if (FT_Set_Char_Size(face, 0, FT_F26Dot6(std::lround(pixelSize * rasterDensity * 64)),
                                 72, 72))
                continue;
            restore.face = face;
        }
        if (FT_Load_Glyph(face, glyph.id, FT_LOAD_DEFAULT | FT_LOAD_COLOR) ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
            continue;
        const auto& bitmap = face->glyph->bitmap;
        if (!bitmap.width || !bitmap.rows)
            continue;
        const float scale = scalable ? glyph.font->scale : glyph.font->scale * rasterDensity;
        const int x = std::lround((glyph.x - layout.left) * rasterDensity +
                                  face->glyph->bitmap_left * scale);
        const int y = std::lround((glyph.y - layout.top) * rasterDensity -
                                  face->glyph->bitmap_top * scale);
        const bool colored = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
        QImage mask(bitmap.width, bitmap.rows,
                    colored ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_Alpha8);
        mask.fill(0);
        for (unsigned row = 0; row < bitmap.rows; ++row) {
            const auto* pixels = bitmap.buffer +
                (bitmap.pitch >= 0 ? row : bitmap.rows - 1 - row) * std::abs(bitmap.pitch);
            auto* output = mask.scanLine(row);
            for (unsigned column = 0; column < bitmap.width; ++column) {
                if (colored) {
                    output[column * 4] = pixels[column * 4 + 2];
                    output[column * 4 + 1] = pixels[column * 4 + 1];
                    output[column * 4 + 2] = pixels[column * 4];
                    output[column * 4 + 3] = pixels[column * 4 + 3];
                } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                    output[column] = pixels[column];
                } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                    output[column] = (pixels[column / 8] & (0x80 >> (column % 8))) ? 255 : 0;
                }
            }
        }
        if (scale != 1) {
            mask = mask.scaled(std::max(1, static_cast<int>(std::lround(mask.width() * scale))),
                               std::max(1, static_cast<int>(std::lround(mask.height() * scale))),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        for (int row = 0; row < mask.height(); ++row) {
            const auto* pixels = mask.constScanLine(row);
            for (int column = 0; column < mask.width(); ++column) {
                if (colored) {
                    const auto* p = pixels + column * 4;
                    if (p[3])
                        image.blend(
                            x + column, y + row,
                            {p[0] / float(p[3]), p[1] / float(p[3]), p[2] / float(p[3]), color.a},
                            p[3] / 255.0f);
                } else {
                    image.blend(x + column, y + row, color, pixels[column] / 255.0f);
                }
            }
        }
    }
    return image;
}

QString TextRenderer::elideText(const QString& text, int pixelSize, int maxWidth)
{
    if (maxWidth <= 0 || pixelSize <= 0 || pixelSize > 128)
        return {};
    const QString line = singleLine(text);
    if (textSize(line, pixelSize).width() <= maxWidth)
        return line;
    const QString ellipsis(QChar(0x2026));
    if (textSize(ellipsis, pixelSize).width() > maxWidth)
        return {};
    const auto breaks = boundaries(line);
    const auto direction =
        ubidi_getBaseDirection(reinterpret_cast<const UChar*>(line.utf16()), line.size());
    int low = 0, high = static_cast<int>(breaks.size()) - 2;
    QString best = ellipsis;
    while (low <= high) {
        const int guess = low + (high - low) / 2;
        const QString candidate = withEllipsis(line.left(breaks[guess]), direction);
        if (textSize(candidate, pixelSize).width() <= maxWidth) {
            best = candidate;
            low = guess + 1;
        } else {
            high = guess - 1;
        }
    }
    return best;
}

} // namespace cutefish::decoration
