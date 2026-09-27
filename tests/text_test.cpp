// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>

#include <QTextBoundaryFinder>

#include "decoration/text.h"

using namespace cutefish::decoration;

namespace {
class TitleText : public testing::Test
{
protected:
    TextRenderer renderer;

    QSize textSize(const QString& text, int size) { return renderer.textSize(text, size); }

    Image renderText(const QString& text, int size, const cutefish::render::Color& color)
    {
        return renderer.renderText(text, size, color);
    }

    QString elideText(const QString& text, int size, int width)
    {
        return renderer.elideText(text, size, width);
    }

    std::vector<uint8_t> pixels(const QString& text)
    {
        const auto image = renderText(text, 14, {1, 1, 1, 1});
        return {image.bits(), image.bits() + image.stride() * image.size().height()};
    }
};
} // namespace

TEST_F(TitleText, EmptyAndInvalidSizesAreSafeWithoutGuiApplication)
{
    EXPECT_TRUE(textSize({}, 14).isEmpty());
    EXPECT_TRUE(textSize(QStringLiteral("Title"), 0).isEmpty());
    EXPECT_TRUE(textSize(QStringLiteral("Title"), -1).isEmpty());
    EXPECT_TRUE(textSize(QStringLiteral("Title"), 129).isEmpty());
    EXPECT_TRUE(elideText(QStringLiteral("Title"), 14, 0).isEmpty());
}

TEST_F(TitleText, RendererLifetimesAreIndependent)
{
    const QString title = QStringLiteral("Editor — 中文 — العربية");
    const auto expected = pixels(title);
    ASSERT_FALSE(expected.empty());
    for (int iteration = 0; iteration < 3; ++iteration) {
        {
            TextRenderer temporary;
            EXPECT_EQ(temporary.textSize(title, 14), textSize(title, 14));
            EXPECT_FALSE(temporary.renderText(title, 18, {1, 1, 1, 1}).size().isEmpty());
            EXPECT_EQ(temporary.textSize(title, 14), textSize(title, 14));
        }
        EXPECT_EQ(pixels(title), expected);
    }
}

TEST_F(TitleText, UsesProportionalSystemGlyphsAndAntialiasing)
{
    EXPECT_GT(textSize(QStringLiteral("WWW"), 14).width(),
              textSize(QStringLiteral("iii"), 14).width());
    const auto image = renderText(QStringLiteral("Title"), 14, {1, 0.5f, 0.25f, 0.5f});
    EXPECT_EQ(image.size(), textSize(QStringLiteral("Title"), 14));
    bool ink = false, antialiasing = false;
    for (int i = 0; i < image.stride() * image.size().height(); i += 4) {
        const uint8_t* p = image.bits() + i;
        EXPECT_LE(p[0], p[3]);
        EXPECT_LE(p[1], p[3]);
        EXPECT_LE(p[2], p[3]);
        EXPECT_LE(p[3], 128);
        ink |= p[3] > 0;
        antialiasing |= p[3] > 0 && p[3] < 120;
    }
    EXPECT_TRUE(ink);
    EXPECT_TRUE(antialiasing);
}

TEST_F(TitleText, UnicodeTitlesAreNotRepeatedPlaceholderBoxes)
{
    // The test environment supplies Noto CJK and DejaVu Arabic/Hebrew fonts.
    EXPECT_NE(pixels(QStringLiteral("中文")), pixels(QStringLiteral("标题")));
    EXPECT_EQ(pixels(QStringLiteral("é")), pixels(QStringLiteral("e\u0301")));
    EXPECT_NE(pixels(QStringLiteral("سلام")), pixels(QStringLiteral("س ل ا م")));
    EXPECT_NE(pixels(QStringLiteral("אבג")), pixels(QStringLiteral("גבא")));
    EXPECT_EQ(pixels(QStringLiteral("אבג")), pixels(QStringLiteral("\u202Dגבא\u202C")));
    const auto mixed = pixels(QStringLiteral("Editor — 中文 — العربية — 😀"));
    EXPECT_TRUE(std::any_of(mixed.begin(), mixed.end(), [](uint8_t b) {
        return b != 0;
    }));
}

TEST_F(TitleText, BidiFormattingDoesNotAddFontMetrics)
{
    for (const QString text : {QStringLiteral("\u202D\u202C"),
                               QStringLiteral("\u2066\u2069"),
                               QStringLiteral("\u200E\u200F\u061C")}) {
        EXPECT_TRUE(textSize(text, 14).isEmpty());
        EXPECT_TRUE(renderText(text, 14, {1, 1, 1, 1}).size().isEmpty());
    }
    const QString hebrew = QStringLiteral("אבג");
    const QString isolated = QStringLiteral("\u2067אבג\u2069");
    EXPECT_EQ(textSize(hebrew, 14), textSize(isolated, 14));
    EXPECT_EQ(pixels(hebrew), pixels(isolated));
    EXPECT_EQ(pixels(hebrew), pixels(QStringLiteral("\u202Dגבא\u202C")));
}

TEST_F(TitleText, ElisionFitsAtGraphemeBoundaries)
{
    const QString title =
        QStringLiteral("A e\u0301 中文 👩‍💻 العربية document title");
    for (int width = 1; width < textSize(title, 14).width(); ++width) {
        const QString elided = elideText(title, 14, width);
        EXPECT_LE(textSize(elided, 14).width(), width);
        if (elided.isEmpty())
            continue;
        const int ellipsis = elided.indexOf(QChar(0x2026));
        ASSERT_GE(ellipsis, 0);
        EXPECT_EQ(elided.left(ellipsis), title.left(ellipsis));
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, title);
        finder.setPosition(ellipsis);
        EXPECT_TRUE(finder.isAtBoundary());
    }
    EXPECT_EQ(elideText(title, 14, 1000), title);
    EXPECT_EQ(elideText(title, 14, textSize(QString(QChar(0x2026)), 14).width()),
              QString(QChar(0x2026)));
}

TEST_F(TitleText, NewlinesStayOnOneLineAndInputWorkIsBounded)
{
    EXPECT_EQ(pixels(QStringLiteral("a\nb\rc\td\u2028e\u2029f")),
              pixels(QStringLiteral("a b c d e f")));
    EXPECT_EQ(textSize(QString(100000, QLatin1Char('a')), 14),
              textSize(QString(4096, QLatin1Char('a')), 14));
}

TEST_F(TitleText, EmojiPresentationUsesColorAndScalesBitmapStrikes)
{
    const auto image = renderText(QStringLiteral("😀👩‍💻"), 14, {1, 1, 1, 1});
    ASSERT_FALSE(image.size().isEmpty());
    EXPECT_LT(image.size().height(), 30);
    bool colored = false;
    for (int i = 0; i < image.stride() * image.size().height(); i += 4) {
        const auto* p = image.bits() + i;
        colored |= p[0] != p[1] || p[1] != p[2];
        EXPECT_LE(p[0], p[3]);
        EXPECT_LE(p[1], p[3]);
        EXPECT_LE(p[2], p[3]);
    }
    EXPECT_TRUE(colored);
}

TEST_F(TitleText, DeviceRasterKeepsLogicalShapingAndElision)
{
    for (const auto& title : {QStringLiteral("Fractional title"), QStringLiteral("العربية 中文"),
                              QStringLiteral("😀👩‍💻")}) {
        const QString elided = elideText(title, 14, 70);
        const QSize logical = textSize(elided, 14);
        for (double density : {1.0, 2.0, 3.0, 4.0, 1.25, 1.5, 1.75, 1.0}) {
            const auto image = renderer.renderText(elided, 14, {1, 1, 1, 1}, density);
            EXPECT_EQ(image.size(), QSize(int(std::ceil(logical.width() * density)),
                                          int(std::ceil(logical.height() * density))))
                    << density;
            const uint8_t *bits = image.bits();
            const size_t bytes = size_t(image.stride()) * image.size().height();
            bool inked = false;
            for (size_t i = 3; i < bytes && !inked; i += 4)
                inked = bits[i] > 0;
            EXPECT_TRUE(inked) << density;
            EXPECT_EQ(textSize(elided, 14), logical);
        }
    }
}
