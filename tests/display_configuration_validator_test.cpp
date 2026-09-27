// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ui/display/manager/configure_displays_task_unittest.cc
// Revision: 7acd40ddefff49c9d36144999aafdbb674dbd07a
//
// Adapted for Cutefish: tests use the revisioned request and structured-error
// contract without Chromium display tasks.

#include <gtest/gtest.h>

#include "display/display_configuration_validator.h"

using namespace cutefish;

namespace {

display::DisplayId id(const char* value)
{
    return display::DisplayId(QString::fromLatin1(value));
}

display::DisplayMode mode(QSize size)
{
    return {size, 60000, 148500, size.width() + 100, size.height() + 50, 0,
            QStringLiteral("mode"), true};
}

display::DisplaySnapshot output(const char* name, QSize size)
{
    display::DisplaySnapshot value;
    value.sessionId = id(name);
    value.persistentKey = QStringLiteral("edid-") + QString::fromLatin1(name);
    value.portKey = QStringLiteral("port-") + QString::fromLatin1(name);
    value.connectorName = QString::fromLatin1(name);
    value.devicePath = QStringLiteral("/dev/dri/card0");
    value.physicalSizeMm = QSize(600, 340);
    value.modes = {mode(size)};
    value.currentMode = value.modes.first();
    value.capabilities = display::DisplayCapability::Scale |
        display::DisplayCapability::Rotation;
    return value;
}

display::DisplayConfiguration configuration(const display::HardwareSnapshot& hardware)
{
    display::DisplayConfiguration value;
    value.base = {hardware.compositorInstanceId, hardware.topologyGeneration, 7};
    value.primary = id("internal");
    value.displays = {{id("internal"), true, hardware.displays[0].modes[0], {},
                       display::DisplayRotation::Normal},
                      {id("external"), true, hardware.displays[1].modes[0],
                       *compositor::OutputScale::fromNumerator(150),
                       display::DisplayRotation::Rotate90}};
    value.placements = {{id("external"), id("internal"), display::LayoutEdge::Right, 0}};
    return value;
}

} // namespace

TEST(DisplayConfigurationValidator, NormalizesAnExactMixedScaleRotatedRequest)
{
    const display::HardwareSnapshot hardware {QStringLiteral("instance"), QStringLiteral("seat0"),
                                              3,
                                              {output("internal", {1920, 1080}),
                                               output("external", {2560, 1440})}};
    const auto result = display::DisplayConfigurationValidator::validateExtended(
        hardware, 7, configuration(hardware));
    ASSERT_TRUE(result.succeeded) << result.error.message.toStdString();
    EXPECT_EQ(result.logicalBounds.value(id("internal")), QRect(0, 0, 1920, 1080));
    EXPECT_EQ(result.logicalBounds.value(id("external")), QRect(1920, 0, 1152, 2048));
}

TEST(DisplayConfigurationValidator, RejectsStaleUnknownUnsupportedAndAllDisabledRequests)
{
    display::HardwareSnapshot hardware {QStringLiteral("instance"), QStringLiteral("seat0"), 3,
                                        {output("internal", {1920, 1080}),
                                         output("external", {2560, 1440})}};
    auto requested = configuration(hardware);
    requested.base.appliedRevision = 6;
    EXPECT_EQ(display::DisplayConfigurationValidator::validateExtended(hardware, 7, requested)
                  .error.code,
              display::DisplayErrorCode::StaleConfiguration);

    requested = configuration(hardware);
    requested.displays[1].display = id("missing");
    EXPECT_EQ(display::DisplayConfigurationValidator::validateExtended(hardware, 7, requested)
                  .error.code,
              display::DisplayErrorCode::UnknownOutput);

    requested = configuration(hardware);
    requested.displays[1].mode.refreshMilliHz = 59940;
    EXPECT_EQ(display::DisplayConfigurationValidator::validateExtended(hardware, 7, requested)
                  .error.code,
              display::DisplayErrorCode::UnsupportedMode);

    // D-Bus carries both as plain integers.
    requested = configuration(hardware);
    requested.displays[1].rotation = display::DisplayRotation(7);
    EXPECT_EQ(display::DisplayConfigurationValidator::validateExtended(hardware, 7, requested)
                  .error.code,
              display::DisplayErrorCode::UnsupportedProperty);
    requested = configuration(hardware);
    requested.placements[0].edge = display::LayoutEdge(9);
    EXPECT_EQ(display::DisplayConfigurationValidator::validateExtended(hardware, 7, requested)
                  .error.code,
              display::DisplayErrorCode::InvalidLayout);

    requested = configuration(hardware);
    for (auto& output : requested.displays)
        output.enabled = false;
    EXPECT_EQ(display::DisplayConfigurationValidator::validateExtended(hardware, 7, requested)
                  .error.code,
              display::DisplayErrorCode::NoEnabledOutput);
}

TEST(DisplayConfigurationValidator, AcceptsDifferentModeSoftwareMirrorDestinations)
{
    display::HardwareSnapshot hardware {QStringLiteral("instance"), QStringLiteral("seat0"), 3,
                                        {output("internal", {1920, 1080}),
                                         output("external", {2560, 1440})}};
    for (auto& display : hardware.displays)
        display.capabilities |= display::DisplayCapability::SoftwareMirror;
    auto requested = configuration(hardware);
    requested.primary = id("internal");
    requested.placements.clear();
    requested.mirrorGroups = {{id("internal"), id("external")}};
    const auto result =
        display::DisplayConfigurationValidator::validateExtended(hardware, 7, requested);
    ASSERT_TRUE(result.succeeded) << result.error.message.toStdString();
    EXPECT_TRUE(result.logicalBounds.contains(id("internal")));
    EXPECT_FALSE(result.logicalBounds.contains(id("external")));
}
