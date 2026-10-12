// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Hot corners: which corners count, when they fire, and their settings.

#include <gtest/gtest.h>

#include <QSettings>
#include <QTemporaryDir>

#include "accelerators/accelerator.h"
#include "accelerators/hot_corner_controller.h"
#include "accelerators/hot_corner_settings.h"

using namespace cutefish::accelerators;

namespace {

constexpr QRect kLeft(0, 0, 1000, 800);
constexpr QRect kRight(1000, 0, 1000, 800);

class HotCornerTest : public testing::Test
{
protected:
    std::vector<AcceleratorAction> performed;
    HotCornerController controller {nullptr, [this](AcceleratorAction action) {
                                        performed.push_back(action);
                                        return true;
                                    }};

    void SetUp() override
    {
        controller.onPointerMoved(QPointF(500, 400));
        controller.setOutputs({kLeft});
        controller.setBinding(HotCorner::TopLeft, {AcceleratorAction::ToggleOverview, NoModifier});
        controller.setBinding(HotCorner::BottomRight,
                              {AcceleratorAction::ToggleShowDesktop, NoModifier});
    }
};

TEST_F(HotCornerTest, OneOutputHasFourCorners)
{
    const auto corners = controller.corners();
    ASSERT_EQ(corners.size(), 4u);
    EXPECT_EQ(corners[0].second, QPoint(0, 0));
    EXPECT_EQ(corners[1].second, QPoint(999, 0));
    EXPECT_EQ(corners[2].second, QPoint(0, 799));
    EXPECT_EQ(corners[3].second, QPoint(999, 799));
}

TEST_F(HotCornerTest, CornersWhereOutputsMeetDoNotCount)
{
    controller.setOutputs({kLeft, kRight});
    const auto corners = controller.corners();
    ASSERT_EQ(corners.size(), 4u);
    EXPECT_EQ(corners[0], std::make_pair(HotCorner::TopLeft, QPoint(0, 0)));
    EXPECT_EQ(corners[1], std::make_pair(HotCorner::BottomLeft, QPoint(0, 799)));
    EXPECT_EQ(corners[2], std::make_pair(HotCorner::TopRight, QPoint(1999, 0)));
    EXPECT_EQ(corners[3], std::make_pair(HotCorner::BottomRight, QPoint(1999, 799)));
}

TEST_F(HotCornerTest, ATallerOutputKeepsTheCornerBesideTheShorterOne)
{
    // The right output reaches further down: its bottom-left corner has
    // nothing to its left or below, so it is a corner too.
    const QRect tall(1000, 0, 1000, 1000);
    controller.setOutputs({kLeft, tall});
    bool found = false;
    for (const auto &[corner, point] : controller.corners()) {
        if (corner == HotCorner::BottomLeft && point == QPoint(1000, 999))
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(HotCornerTest, FiresOnArrival)
{
    controller.onPointerMoved(QPointF(10, 10));
    EXPECT_TRUE(performed.empty());
    controller.onPointerMoved(QPointF(0, 0));
    EXPECT_EQ(performed, std::vector {AcceleratorAction::ToggleOverview});
    controller.onPointerMoved(QPointF(999.5, 799.5));
    EXPECT_EQ(performed.back(), AcceleratorAction::ToggleShowDesktop);
}

TEST_F(HotCornerTest, FiresOncePerVisit)
{
    controller.onPointerMoved(QPointF(0, 0));
    // Jitter around the corner, then back into it.
    controller.onPointerMoved(QPointF(3, 5));
    controller.onPointerMoved(QPointF(0, 0));
    EXPECT_EQ(performed.size(), 1u);
    // Out of its neighbourhood and back.
    controller.onPointerMoved(QPointF(HotCornerController::kRearmDistance + 1, 0));
    controller.onPointerMoved(QPointF(0, 0));
    EXPECT_EQ(performed.size(), 2u);
}

TEST_F(HotCornerTest, ADragUsesTheVisitUp)
{
    controller.onPointerMoved(QPointF(0, 0), true);
    EXPECT_TRUE(performed.empty());
    // Released in the corner: it waits for the pointer to leave first.
    controller.onPointerMoved(QPointF(0, 0));
    EXPECT_TRUE(performed.empty());
    controller.onPointerMoved(QPointF(HotCornerController::kRearmDistance + 1, 0));
    controller.onPointerMoved(QPointF(0, 0));
    EXPECT_EQ(performed.size(), 1u);
}

TEST_F(HotCornerTest, ACornerWithoutAnActionDoesNothing)
{
    controller.onPointerMoved(QPointF(999, 0));
    controller.onPointerMoved(QPointF(0, 799));
    EXPECT_TRUE(performed.empty());
}

TEST_F(HotCornerTest, ARequiredModifierMustBeHeld)
{
    // Nothing is held without a seat.
    controller.setBinding(HotCorner::TopLeft, {AcceleratorAction::ToggleOverview, AltDown});
    controller.onPointerMoved(QPointF(0, 0));
    EXPECT_TRUE(performed.empty());
}

TEST_F(HotCornerTest, TheCornerUnderThePointerWaitsForItToLeave)
{
    controller.onPointerMoved(QPointF(0, 0));
    performed.clear();
    // An output change while the pointer rests in the corner.
    controller.setOutputs({kLeft});
    controller.onPointerMoved(QPointF(0, 0));
    EXPECT_TRUE(performed.empty());
}

TEST(HotCornerSettingsTest, KeepsCornersInTheSettingsFile)
{
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("compositor.conf"));
    {
        HotCornerSettings settings(nullptr, file);
        EXPECT_FALSE(settings.binding(HotCorner::TopLeft).action);
        settings.setCorner(QStringLiteral("top-left"), QStringLiteral("toggle-overview"), 0);
        settings.setCorner(QStringLiteral("bottom-right"), QStringLiteral("lock-screen"),
                           int(ControlDown | AltGrDown));
        settings.setCorner(QStringLiteral("nowhere"), QStringLiteral("lock-screen"), 0);
        EXPECT_EQ(settings.topLeft(), QStringLiteral("toggle-overview"));
        // AltGr is not something a corner can ask for.
        EXPECT_EQ(settings.bottomRightModifiers(), int(ControlDown));
    }
    HotCornerSettings reloaded(nullptr, file);
    EXPECT_EQ(reloaded.binding(HotCorner::TopLeft),
              (HotCornerBinding {AcceleratorAction::ToggleOverview, NoModifier}));
    EXPECT_EQ(reloaded.binding(HotCorner::BottomRight),
              (HotCornerBinding {AcceleratorAction::LockScreen, ControlDown}));

    // An unknown action turns the corner off and leaves no keys behind.
    reloaded.setCorner(QStringLiteral("top-left"), QStringLiteral("no-such-action"), 0);
    EXPECT_TRUE(reloaded.topLeft().isEmpty());
    QSettings raw(file, QSettings::IniFormat);
    EXPECT_FALSE(raw.contains(QStringLiteral("HotCorners/TopLeft")));
    EXPECT_EQ(raw.value(QStringLiteral("HotCorners/BottomRight")).toString(),
              QStringLiteral("lock-screen"));
}

} // namespace
