#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "editor/brush/brushProfile.h"
#include "editor/brush/brushSampler.h"

namespace
{

using namespace VL::Editor;

BrushProfile WeightProfile()
{
    BrushProfile profile;
    profile.radiusCm = 100.0f;
    profile.strength = 1.0f;
    return profile;
}

} // namespace

TEST(BrushSamplerTest, SmoothFalloffUsesInvertedSmoothstep)
{
    BrushProfile profile = WeightProfile();
    profile.falloff = BrushFalloff::Smooth;

    EXPECT_NEAR(SampleWeight(profile, 0.0f), 1.0f, 1.0e-6f);
    EXPECT_NEAR(SampleWeight(profile, 0.5f), 0.5f, 1.0e-6f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 1.25f), 0.0f);
}

TEST(BrushSamplerTest, LinearFalloffFadesToEdge)
{
    BrushProfile profile = WeightProfile();
    profile.falloff = BrushFalloff::Linear;

    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.25f), 0.75f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 2.0f), 0.0f);
}

TEST(BrushSamplerTest, ConstantFalloffIsFullInsideAndZeroOutside)
{
    BrushProfile profile = WeightProfile();
    profile.falloff = BrushFalloff::Constant;

    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.99f), 1.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 1.5f), 0.0f);
}

TEST(BrushSamplerTest, SquareShapeAndRotationUseTwoDimensionalOffset)
{
    BrushProfile square = WeightProfile();
    square.shape = BrushShape::Square;
    square.falloff = BrushFalloff::Constant;

    // Chebyshev 距离：对角线角落仍在方形刷内，圆形刷则已经在半径外。
    EXPECT_FLOAT_EQ(
        SampleWeightAt(square, Eigen::Vector2f(0.9f, 0.9f)), 1.0f);
    BrushProfile circle = square;
    circle.shape = BrushShape::Circle;
    EXPECT_FLOAT_EQ(
        SampleWeightAt(circle, Eigen::Vector2f(0.9f, 0.9f)), 0.0f);

    // 旋转 45° 后同一角落落在外接半径之外；旋转后的轴上点仍在方形内。
    square.rotationDeg = 45.0f;
    EXPECT_FLOAT_EQ(
        SampleWeightAt(square, Eigen::Vector2f(0.9f, 0.9f)), 0.0f);
    EXPECT_FLOAT_EQ(
        SampleWeightAt(square, Eigen::Vector2f(0.5f, -0.5f)), 1.0f);
}

TEST(BrushSamplerTest, LutFalloffInterpolatesSamples)
{
    BrushProfile profile = WeightProfile();
    profile.falloff = BrushFalloff::Lut;
    profile.falloffLut = {1.0f, 0.5f, 0.0f};

    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.25f), 0.75f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.75f), 0.25f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 1.0f), 0.0f);
}

TEST(BrushSamplerTest, EffectiveRadiusUsesClampedPressureSizeInfluence)
{
    BrushProfile profile = WeightProfile();
    profile.pressureSizeInfluence = 0.5f;

    EXPECT_FLOAT_EQ(EffectiveRadiusCm(profile, 1.0f), 100.0f);
    EXPECT_FLOAT_EQ(EffectiveRadiusCm(profile, 0.5f), 75.0f);
    EXPECT_FLOAT_EQ(EffectiveRadiusCm(profile, -1.0f), 50.0f);
    EXPECT_FLOAT_EQ(EffectiveRadiusCm(profile, 2.0f), 100.0f);

    profile.pressureSizeInfluence = 0.0f;
    EXPECT_FLOAT_EQ(EffectiveRadiusCm(profile, 0.0f), 100.0f);
}

TEST(BrushSamplerTest, WeightUsesClampedPressureStrengthInfluence)
{
    BrushProfile profile = WeightProfile();
    profile.falloff = BrushFalloff::Linear;
    profile.strength = 0.8f;
    profile.pressureStrengthInfluence = 1.0f;

    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.5f, 1.0f), 0.4f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.5f, 0.5f), 0.2f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.5f, -1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.5f, 2.0f), 0.4f);

    profile.pressureStrengthInfluence = 0.0f;
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.5f, 0.0f), 0.4f);
}

TEST(BrushSamplerTest, NoiseModulatesWeightAndClampsSample)
{
    BrushProfile profile = WeightProfile();
    profile.noise = 0.5f;

    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f, 1.0f, 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f, 1.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f, 1.0f, 7.0f), 1.0f);
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f, 1.0f, -7.0f), 0.5f);

    profile.noise = 0.0f;
    EXPECT_FLOAT_EQ(SampleWeight(profile, 0.0f, 1.0f, 0.0f), 1.0f);
}

TEST(BrushSamplerTest, StampSpacingIsPositiveAndScalesWithRadiusAndPressure)
{
    BrushProfile profile = WeightProfile();
    profile.spacing = 0.5f;
    EXPECT_FLOAT_EQ(StampSpacingCm(profile, 1.0f), 100.0f);

    profile.pressureSizeInfluence = 1.0f;
    EXPECT_FLOAT_EQ(StampSpacingCm(profile, 0.5f), 50.0f);

    profile.radiusCm = 1.0e-6f;
    profile.spacing = 1.0e-6f;
    EXPECT_GT(StampSpacingCm(profile, 0.0f), 0.0f);
}

TEST(BrushSamplerTest, JitteredStampCenterIsDeterministicAndZeroWithoutJitter)
{
    BrushProfile profile = WeightProfile();
    const Eigen::Vector2f center(12.0f, -8.0f);
    const Eigen::Vector2f fixed = JitteredStampCenter(profile, center, 17u);
    EXPECT_FLOAT_EQ(fixed.x(), center.x());
    EXPECT_FLOAT_EQ(fixed.y(), center.y());

    profile.jitter = 0.5f;
    profile.noiseSeed = 1234u;
    const Eigen::Vector2f first = JitteredStampCenter(profile, center, 17u);
    const Eigen::Vector2f second = JitteredStampCenter(profile, center, 17u);
    EXPECT_FLOAT_EQ(first.x(), second.x());
    EXPECT_FLOAT_EQ(first.y(), second.y());
    EXPECT_LE(std::abs(first.x() - center.x()), 50.0f);
    EXPECT_LE(std::abs(first.y() - center.y()), 50.0f);
}

TEST(BrushSamplerTest, StableNoiseIsDeterministicAndBounded)
{
    const float first = StableNoise01(3u, 5u, 7u);
    const float second = StableNoise01(3u, 5u, 7u);
    EXPECT_FLOAT_EQ(first, second);
    EXPECT_GE(first, 0.0f);
    EXPECT_LE(first, 1.0f);
    EXPECT_GE(StableNoise01(0u, 0u, 0u), 0.0f);
    EXPECT_LE(StableNoise01(0u, 0u, 0u), 1.0f);
}

TEST(BrushProfileTest, JsonRoundTripsAllFields)
{
    BrushProfile source;
    source.name = "Custom Stamp";
    source.category = "Testing";
    source.shape = BrushShape::Stamp;
    source.falloff = BrushFalloff::Lut;
    source.radiusCm = 321.5f;
    source.strength = 0.42f;
    source.spacing = 0.35f;
    source.jitter = 0.2f;
    source.rotationDeg = 45.5f;
    source.noise = 0.3f;
    source.noiseSeed = 4000000000u;
    source.pressureSizeInfluence = 0.6f;
    source.pressureStrengthInfluence = 0.7f;
    source.falloffLut = {1.0f, 0.75f, 0.2f, 0.0f};
    source.stampPath = "Brushes/stamps/custom.png";

    std::string error;
    ASSERT_TRUE(Validate(source, error)) << error;
    const nlohmann::json json = ToJson(source);
    EXPECT_EQ(json.at("shape"), "stamp");
    EXPECT_EQ(json.at("falloff"), "lut");

    BrushProfile restored;
    ASSERT_TRUE(FromJson(json, restored, error)) << error;
    EXPECT_EQ(restored.name, source.name);
    EXPECT_EQ(restored.category, source.category);
    EXPECT_EQ(restored.shape, source.shape);
    EXPECT_EQ(restored.falloff, source.falloff);
    EXPECT_FLOAT_EQ(restored.radiusCm, source.radiusCm);
    EXPECT_FLOAT_EQ(restored.strength, source.strength);
    EXPECT_FLOAT_EQ(restored.spacing, source.spacing);
    EXPECT_FLOAT_EQ(restored.jitter, source.jitter);
    EXPECT_FLOAT_EQ(restored.rotationDeg, source.rotationDeg);
    EXPECT_FLOAT_EQ(restored.noise, source.noise);
    EXPECT_EQ(restored.noiseSeed, source.noiseSeed);
    EXPECT_FLOAT_EQ(
        restored.pressureSizeInfluence,
        source.pressureSizeInfluence);
    EXPECT_FLOAT_EQ(
        restored.pressureStrengthInfluence,
        source.pressureStrengthInfluence);
    EXPECT_EQ(restored.falloffLut, source.falloffLut);
    EXPECT_EQ(restored.stampPath, source.stampPath);
}

TEST(BrushProfileTest, JsonIgnoresUnknownKeysAndRejectsWrongTypes)
{
    const BrushProfile source = SculptBrushProfiles().front();
    nlohmann::json json = ToJson(source);
    json["futureField"] = nlohmann::json{{"ignored", true}};

    BrushProfile restored;
    std::string error;
    EXPECT_TRUE(FromJson(json, restored, error)) << error;

    json = ToJson(source);
    json["radiusCm"] = "wide";
    EXPECT_FALSE(FromJson(json, restored, error));
    EXPECT_NE(error.find("radiusCm"), std::string::npos) << error;

    json = ToJson(source);
    json["noiseSeed"] = 1.5;
    EXPECT_FALSE(FromJson(json, restored, error));
    EXPECT_NE(error.find("noiseSeed"), std::string::npos) << error;

    json = ToJson(source);
    json["shape"] = 4;
    EXPECT_FALSE(FromJson(json, restored, error));
    EXPECT_NE(error.find("shape"), std::string::npos) << error;
}

TEST(BrushProfileTest, ValidateRejectsInvalidRadius)
{
    BrushProfile profile;
    std::string error;

    profile.radiusCm = -1.0f;
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("radiusCm"), std::string::npos) << error;

    profile.radiusCm = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("radiusCm"), std::string::npos) << error;
}

TEST(BrushProfileTest, ValidateRejectsOutOfRangeScalars)
{
    BrushProfile profile;
    std::string error;

    profile.strength = -0.1f;
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("strength"), std::string::npos) << error;
    profile = BrushProfile{};

    profile.spacing = 0.0f;
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("spacing"), std::string::npos) << error;
    profile = BrushProfile{};

    profile.jitter = 1.1f;
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("jitter"), std::string::npos) << error;
    profile = BrushProfile{};

    profile.noise = -0.1f;
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("noise"), std::string::npos) << error;
    profile = BrushProfile{};

    profile.pressureSizeInfluence = 1.1f;
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("pressureSizeInfluence"), std::string::npos) << error;
    profile = BrushProfile{};

    profile.pressureStrengthInfluence = -0.1f;
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(
        error.find("pressureStrengthInfluence"),
        std::string::npos) << error;
}

TEST(BrushProfileTest, ValidateRejectsInvalidLut)
{
    BrushProfile profile;
    profile.falloff = BrushFalloff::Lut;
    std::string error;

    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("falloffLut"), std::string::npos) << error;

    profile.falloffLut = {1.0f};
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("falloffLut"), std::string::npos) << error;

    profile.falloffLut = {1.0f, 1.5f};
    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("falloffLut"), std::string::npos) << error;
}

TEST(BrushProfileTest, ValidateRejectsStampWithoutPath)
{
    BrushProfile profile;
    profile.shape = BrushShape::Stamp;
    std::string error;

    EXPECT_FALSE(Validate(profile, error));
    EXPECT_NE(error.find("stampPath"), std::string::npos) << error;
}

TEST(BrushProfileTest, FromJsonRejectsInvalidSerializedProfile)
{
    nlohmann::json json = ToJson(SculptBrushProfiles().front());
    json["radiusCm"] = -5.0f;

    BrushProfile profile;
    std::string error;
    EXPECT_FALSE(FromJson(json, profile, error));
    EXPECT_NE(error.find("radiusCm"), std::string::npos) << error;
}

TEST(BrushProfileTest, SculptCatalogValidatesAndHasUniqueIdsAndNames)
{
    const std::vector<BrushProfile> profiles = SculptBrushProfiles();
    ASSERT_GE(profiles.size(), 4u);

    std::set<std::string> names;
    std::set<std::string> ids;
    bool foundSoft = false;
    bool foundHard = false;
    bool foundWide = false;
    bool foundSquare = false;
    for (const BrushProfile& profile : profiles)
    {
        std::string error;
        EXPECT_TRUE(Validate(profile, error)) << profile.name << ": " << error;
        EXPECT_EQ(profile.category, "Sculpt");
        EXPECT_TRUE(names.insert(profile.name).second) << profile.name;
        EXPECT_TRUE(ids.insert(BrushProfileId(profile)).second)
            << BrushProfileId(profile);
        EXPECT_GT(profile.radiusCm, 0.0f);
        EXPECT_GE(profile.strength, 0.0f);
        EXPECT_LE(profile.strength, 1.0f);

        if (profile.name == "Soft Round")
        {
            foundSoft = true;
            EXPECT_EQ(profile.falloff, BrushFalloff::Smooth);
        }
        else if (profile.name == "Hard Round")
        {
            foundHard = true;
            EXPECT_EQ(profile.falloff, BrushFalloff::Constant);
        }
        else if (profile.name == "Wide Soft")
        {
            foundWide = true;
            EXPECT_EQ(profile.shape, BrushShape::Circle);
            EXPECT_EQ(profile.falloff, BrushFalloff::Smooth);
            EXPECT_GT(profile.radiusCm, 500.0f);
        }
        else if (profile.name == "Square")
        {
            foundSquare = true;
            EXPECT_EQ(profile.shape, BrushShape::Square);
            EXPECT_EQ(profile.falloff, BrushFalloff::Linear);
        }
    }

    EXPECT_TRUE(foundSoft);
    EXPECT_TRUE(foundHard);
    EXPECT_TRUE(foundWide);
    EXPECT_TRUE(foundSquare);
}

TEST(BrushProfileTest, PaintCatalogValidatesPresetsAndHasUniqueIdsAndNames)
{
    const std::vector<PaintBrushPreset> presets = PaintBrushProfiles();
    ASSERT_GE(presets.size(), 4u);

    std::set<std::string> names;
    std::set<std::string> ids;
    bool foundSoft = false;
    bool foundHard = false;
    bool foundSpray = false;
    bool foundSquare = false;
    for (const PaintBrushPreset& preset : presets)
    {
        std::string error;
        EXPECT_TRUE(Validate(preset, error))
            << preset.profile.name << ": " << error;
        EXPECT_TRUE(names.insert(preset.profile.name).second)
            << preset.profile.name;
        EXPECT_TRUE(ids.insert(BrushProfileId(preset.profile)).second)
            << BrushProfileId(preset.profile);
        EXPECT_EQ(preset.profile.category, "Paint");
        EXPECT_FLOAT_EQ(preset.profile.strength, 1.0f);
        EXPECT_GT(preset.profile.radiusCm, 0.0f);
        EXPECT_GE(preset.hardness, 0.0f);
        EXPECT_LE(preset.hardness, 1.0f);
        EXPECT_GE(preset.opacity, 0.0f);
        EXPECT_LE(preset.opacity, 1.0f);
        EXPECT_GE(preset.flow, 0.0f);
        EXPECT_LE(preset.flow, 1.0f);

        if (preset.profile.name == "Soft Mask")
        {
            foundSoft = true;
            EXPECT_EQ(preset.profile.shape, BrushShape::Circle);
            EXPECT_EQ(preset.profile.falloff, BrushFalloff::Smooth);
        }
        else if (preset.profile.name == "Hard Mask")
        {
            foundHard = true;
            EXPECT_EQ(preset.profile.falloff, BrushFalloff::Constant);
            EXPECT_FLOAT_EQ(preset.hardness, 1.0f);
        }
        else if (preset.profile.name == "Spray")
        {
            foundSpray = true;
            EXPECT_GT(preset.profile.noise, 0.0f);
        }
        else if (preset.profile.name == "Square Mask")
        {
            foundSquare = true;
            EXPECT_EQ(preset.profile.shape, BrushShape::Square);
            EXPECT_EQ(preset.profile.falloff, BrushFalloff::Linear);
        }
    }

    EXPECT_TRUE(foundSoft);
    EXPECT_TRUE(foundHard);
    EXPECT_TRUE(foundSpray);
    EXPECT_TRUE(foundSquare);
}

TEST(BrushProfileTest, PaintPresetValidateRejectsOutOfRangeMaskValues)
{
    PaintBrushPreset preset = PaintBrushProfiles().front();
    std::string error;

    preset.hardness = 1.1f;
    EXPECT_FALSE(Validate(preset, error));
    EXPECT_NE(error.find("hardness"), std::string::npos) << error;
    preset = PaintBrushProfiles().front();

    preset.opacity = -0.1f;
    EXPECT_FALSE(Validate(preset, error));
    EXPECT_NE(error.find("opacity"), std::string::npos) << error;
    preset = PaintBrushProfiles().front();

    preset.flow = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(Validate(preset, error));
    EXPECT_NE(error.find("flow"), std::string::npos) << error;
}
