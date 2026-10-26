// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "shell/spotlight/spotlight_evaluator.h"

using namespace cutefish::shell;

TEST(SpotlightEvaluatorTest, BasicArithmetic)
{
    auto r1 = SpotlightEvaluator::evaluate("2 + 2");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->result, "4");
    EXPECT_EQ(r1->formatted, "= 4");

    auto r2 = SpotlightEvaluator::evaluate("15 * 8");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->result, "120");

    auto r3 = SpotlightEvaluator::evaluate("100 / 4");
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->result, "25");

    auto r4 = SpotlightEvaluator::evaluate("(12 + 8) * 3");
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4->result, "60");

    auto r5 = SpotlightEvaluator::evaluate("2^10");
    ASSERT_TRUE(r5.has_value());
    EXPECT_EQ(r5->result, "1,024");
}

TEST(SpotlightEvaluatorTest, FunctionsAndPercentage)
{
    auto r1 = SpotlightEvaluator::evaluate("sqrt(144)");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->result, "12");

    auto r2 = SpotlightEvaluator::evaluate("20% of 500");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->result, "100");

    auto r3 = SpotlightEvaluator::evaluate("15% * 200");
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->result, "30");
}

TEST(SpotlightEvaluatorTest, UnitConversions)
{
    auto r1 = SpotlightEvaluator::evaluate("100 c in f");
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->isConversion);
    EXPECT_EQ(r1->result, "212 °F");

    auto r2 = SpotlightEvaluator::evaluate("16 gb to mb");
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(r2->isConversion);
    EXPECT_EQ(r2->result, "16,384 MB");

    auto r3 = SpotlightEvaluator::evaluate("1 km to m");
    ASSERT_TRUE(r3.has_value());
    EXPECT_TRUE(r3->isConversion);
    EXPECT_EQ(r3->result, "1,000 m");
}

TEST(SpotlightEvaluatorTest, NonMathRejection)
{
    EXPECT_FALSE(SpotlightEvaluator::evaluate("firefox").has_value());
    EXPECT_FALSE(SpotlightEvaluator::evaluate("terminal").has_value());
    EXPECT_FALSE(SpotlightEvaluator::evaluate("cutefish").has_value());
    EXPECT_FALSE(SpotlightEvaluator::evaluate("").has_value());
}
