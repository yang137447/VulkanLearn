#include <cmath>

#include <gtest/gtest.h>

#include "render/camera/cameraExposure.h"
#include "render/lighting/physicalLighting.h"

namespace VL
{

TEST(PhysicalLighting, RejectsMissingWorldUnit)
{
    EXPECT_THROW(
        ValidateWorldUnit(nlohmann::json::object()),
        std::invalid_argument);
}

TEST(PhysicalLighting, ConvertsPointLumensToCandela)
{
    const nlohmann::json light = {
        {"type", "pointLight"},
        {"intensityUnit", "lm"},
        {"intensity", 4.0 * LightingPi},
        {"color", {1.0, 1.0, 1.0}}
    };

    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(light), 1.0f);
}

TEST(PhysicalLighting, ConvertsSpotLumensUsingItsEffectiveSolidAngle)
{
    const double inner = 10.0 * LightingPi / 180.0;
    const double outer = 30.0 * LightingPi / 180.0;
    const double solidAngle = SpotEffectiveSolidAngle(inner, outer);
    const nlohmann::json light = {
        {"type", "spotLight"},
        {"intensityUnit", "lm"},
        {"intensity", solidAngle},
        {"color", {1.0, 1.0, 1.0}},
        {"cone_angle_inner", 10.0},
        {"cone_angle_outer", 30.0}
    };

    EXPECT_NEAR(ReadCanonicalLightIntensity(light), 1.0f, 1.0e-6f);
}

TEST(PhysicalLighting, ConvertsSquareLumensUsingAreaInSquareMeters)
{
    const nlohmann::json light = {
        {"type", "squareAreaLight"},
        {"intensityUnit", "lm"},
        {"intensity", 8000.0 * LightingPi * 4.0},
        {"size", 200.0},
        {"color", {1.0, 1.0, 1.0}}
    };

    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(light), 8000.0f);
}

TEST(PhysicalLighting, PacksCandelaForCentimeterDistanceSquared)
{
    EXPECT_FLOAT_EQ(CandelaToCentimeterDistanceIntensity(2.0f), 20000.0f);
    EXPECT_FLOAT_EQ(CandelaToCentimeterDistanceIntensity(0.0f), 0.0f);
}

TEST(PhysicalLighting, CentimeterAttenuationPreservesTheSoftenedMeterKernel)
{
    constexpr float candela = 17.0f;
    for (const float distanceCm : {0.0f, 0.1f, 1.0f, 10.0f, 100.0f, 10000.0f})
    {
        SCOPED_TRACE(distanceCm);
        const float distanceMeters = distanceCm / static_cast<float>(WorldUnitsPerMeter);
        const float reference = candela / (distanceMeters * distanceMeters + 1.0e-4f);
        const float actual = CandelaToCentimeterDistanceIntensity(candela) /
            (distanceCm * distanceCm + 1.0f);
        EXPECT_NEAR(actual, reference, reference * 1.0e-6f);
    }
}

TEST(CameraExposure, NeutralPlotExposurePreservesAuthoredColor)
{
    CameraExposure exposure;
    exposure.ev100 = -std::log2(1.2);
    EXPECT_FLOAT_EQ(ExposureScale(exposure), 1.0f);
}

TEST(PhysicalLighting, RejectsUnitlessAndInvalidSpotAngles)
{
    const nlohmann::json unitless = {
        {"type", "pointLight"},
        {"intensityUnit", "unitless"},
        {"intensity", 1.0},
        {"color", {1.0, 1.0, 1.0}}
    };
    EXPECT_THROW(ReadCanonicalLightIntensity(unitless), std::invalid_argument);

    const nlohmann::json reversed = {
        {"type", "spotLight"},
        {"intensityUnit", "cd"},
        {"intensity", 1.0},
        {"color", {1.0, 1.0, 1.0}},
        {"cone_angle_inner", 30.0},
        {"cone_angle_outer", 10.0}
    };
    EXPECT_THROW(ReadCanonicalLightIntensity(reversed), std::invalid_argument);
}

TEST(CameraExposure, UsesTheFrozenFormulaAndPositiveCompensationBrightens)
{
    CameraExposure exposure;
    exposure.mode = ExposureMode::Ev100;
    exposure.ev100 = 10.0;
    exposure.compensation = 0.0;
    const float base = ExposureScale(exposure);

    exposure.compensation = 1.0;
    const float brighter = ExposureScale(exposure);

    EXPECT_NEAR(base, 1.0f / (1.2f * 1024.0f), 1.0e-8f);
    EXPECT_NEAR(brighter, base * 2.0f, 1.0e-8f);
}

TEST(CameraExposure, PhysicalCameraModeComputesEv100)
{
    CameraExposure exposure;
    exposure.mode = ExposureMode::PhysicalCamera;
    exposure.aperture = 4.0;
    exposure.shutterSeconds = 1.0 / 125.0;
    exposure.iso = 100.0;

    EXPECT_NEAR(EffectiveEv100(exposure), std::log2(2000.0), 1.0e-12);
}

TEST(CameraExposure, AutoModeAcceptsAdaptationContract)
{
    const nlohmann::json camera = {
        {"exposure", {
            {"mode", "auto"},
            {"ev100", 9.0},
            {"aperture", 4.0},
            {"shutterSeconds", 0.008},
            {"iso", 100.0},
            {"compensation", 1.0},
            {"autoMinEv100", -8.0},
            {"autoMaxEv100", 14.0},
            {"autoSpeedUp", 4.0},
            {"autoSpeedDown", 1.5},
            {"autoTargetGray", 0.18}
        }}
    };

    const CameraExposure exposure = ReadCameraExposure(camera);
    EXPECT_EQ(exposure.mode, ExposureMode::Auto);
    EXPECT_FLOAT_EQ(
        static_cast<float>(exposure.autoTargetGray),
        0.18f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(exposure.autoSpeedUp),
        4.0f);
}

TEST(CameraExposure, ManualAutoRoundTripPreservesAuthoredEvAndBounds)
{
    CameraExposure exposure;
    exposure = EditCameraExposure(exposure, "ev100", 2.0);
    exposure = EditCameraExposure(exposure, "autoMinEv100", -1.0);
    exposure = EditCameraExposure(exposure, "mode", 2.0);
    EXPECT_EQ(exposure.mode, ExposureMode::Auto);
    exposure = EditCameraExposure(exposure, "mode", 0.0);
    EXPECT_EQ(exposure.mode, ExposureMode::Ev100);
    EXPECT_DOUBLE_EQ(exposure.ev100, 2.0);
    EXPECT_DOUBLE_EQ(exposure.autoMinEv100, -1.0);
    EXPECT_DOUBLE_EQ(exposure.autoMaxEv100, 20.0);
}

TEST(CameraExposure, ManualEvEditChangesExposureByOneStop)
{
    const CameraExposure exposure;
    const CameraExposure edited = EditCameraExposure(exposure, "ev100", 8.0);
    EXPECT_FLOAT_EQ(ExposureScale(edited), ExposureScale(exposure) * 2.0f);
    EXPECT_DOUBLE_EQ(exposure.ev100, 9.0);
}

TEST(CameraExposure, RejectsInvertedAutoBoundsWithoutChangingOriginal)
{
    const CameraExposure exposure;
    EXPECT_THROW(
        EditCameraExposure(exposure, "autoMinEv100", exposure.autoMaxEv100),
        std::invalid_argument);
    EXPECT_THROW(
        EditCameraExposure(exposure, "autoMaxEv100", exposure.autoMinEv100),
        std::invalid_argument);
    EXPECT_DOUBLE_EQ(exposure.autoMinEv100, -10.0);
    EXPECT_DOUBLE_EQ(exposure.autoMaxEv100, 20.0);
}

TEST(CameraExposure, RejectsInvalidRuntimeFieldsAndNonFiniteValues)
{
    const CameraExposure exposure;
    EXPECT_THROW(EditCameraExposure(exposure, "mode", 3), std::invalid_argument);
    EXPECT_THROW(EditCameraExposure(exposure, "ev100", 33), std::invalid_argument);
    EXPECT_THROW(EditCameraExposure(exposure, "unknown", 1), std::invalid_argument);
    EXPECT_THROW(
        EditCameraExposure(exposure, "compensation", std::nan("")),
        std::invalid_argument);
}

} // namespace VL
