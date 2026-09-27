// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/display_layout_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: uses opaque IDs, Qt geometry, and strict graph errors.

#include <gtest/gtest.h>

#include "display/display_layout.h"

using namespace cutefish::display;

namespace {

DisplayId id(const char* value)
{
    return DisplayId(QString::fromLatin1(value));
}

} // namespace

TEST(DisplayLayout, ResolvesNegativeCoordinatesAndNormalizesSharedEdge)
{
    const QHash<DisplayId, QSize> sizes {
        {id("internal"), {100, 80}}, {id("left"), {60, 120}}, {id("top"), {50, 50}}};
    const QList<DisplayPlacement> placements {
        {id("left"), id("internal"), LayoutEdge::Left, -1000},
        {id("top"), id("internal"), LayoutEdge::Top, 80},
    };
    const auto result = DisplayLayout::resolve(id("internal"), sizes, placements);
    ASSERT_TRUE(result.succeeded) << result.error.message.toStdString();
    EXPECT_EQ(result.bounds.value(id("internal")), QRect(0, 0, 100, 80));
    EXPECT_EQ(result.bounds.value(id("left")), QRect(-60, -88, 60, 120));
    EXPECT_EQ(result.bounds.value(id("top")), QRect(68, -50, 50, 50));
    EXPECT_EQ(result.placements.at(0).offset, -88);
    EXPECT_EQ(result.placements.at(1).offset, 68);
}

TEST(DisplayLayout, RerootsTheSameAdjacencyGraphAtNewPrimary)
{
    const QHash<DisplayId, QSize> sizes {
        {id("a"), {100, 100}}, {id("b"), {80, 60}}, {id("c"), {40, 40}}};
    const QList<DisplayPlacement> placements {
        {id("b"), id("a"), LayoutEdge::Right, 20},
        {id("c"), id("b"), LayoutEdge::Bottom, 10},
    };
    const auto fromA = DisplayLayout::resolve(id("a"), sizes, placements);
    const auto fromB = DisplayLayout::resolve(id("b"), sizes, placements);
    ASSERT_TRUE(fromA.succeeded);
    ASSERT_TRUE(fromB.succeeded);
    EXPECT_EQ(fromB.bounds.value(id("b")).topLeft(), QPoint(0, 0));
    const QPoint translation = fromA.bounds.value(id("b")).topLeft();
    for (const DisplayId& display : sizes.keys())
        EXPECT_EQ(fromB.bounds.value(display).topLeft(),
                  fromA.bounds.value(display).topLeft() - translation);
}

TEST(DisplayLayout, RejectsDisconnectedCyclesMissingIdsAndOverlap)
{
    const QHash<DisplayId, QSize> sizes {
        {id("a"), {100, 100}}, {id("b"), {100, 100}}, {id("c"), {100, 100}}};
    EXPECT_FALSE(DisplayLayout::resolve(id("a"), sizes,
                                        {{id("b"), id("a"), LayoutEdge::Right, 0},
                                         {id("a"), id("b"), LayoutEdge::Left, 0}})
                     .succeeded);
    EXPECT_FALSE(DisplayLayout::resolve(id("a"), sizes,
                                        {{id("b"), id("missing"), LayoutEdge::Right, 0},
                                         {id("c"), id("a"), LayoutEdge::Left, 0}})
                     .succeeded);
    EXPECT_FALSE(DisplayLayout::resolve(id("a"), sizes,
                                        {{id("b"), id("a"), LayoutEdge::Right, 0},
                                         {id("c"), id("a"), LayoutEdge::Right, 0}})
                     .succeeded);
}

TEST(DisplayLayout, SupportsSingleDisplay)
{
    const auto result = DisplayLayout::resolve(id("only"), {{id("only"), {1920, 1080}}}, {});
    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.bounds.value(id("only")), QRect(0, 0, 1920, 1080));
}
