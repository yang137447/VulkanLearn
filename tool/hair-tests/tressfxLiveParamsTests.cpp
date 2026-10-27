#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "hair/tressfxLiveParams.h"
#include "hair/tressfxLiveParamsApply.h"
#include "hair/tressfxSimulationOwner.h"

namespace
{
using VL::Hair::ApplyHairLiveParameter;
using VL::Hair::FindHairLiveParameterSpec;
using VL::Hair::HairLiveParameterRequest;
using VL::Hair::IsHairReloadOnlyField;
using VL::Hair::ParseHairSetArguments;
using VL::Hair::ValidateHairLiveParameterRequest;
using VL::Hair::ValidateHairLiveParameterValues;

HairLiveParameterRequest MakeRequest(const char* objectName, const char* field,
    std::initializer_list<float> values)
{
    HairLiveParameterRequest request;
    request.objectName = objectName;
    request.field = field;
    uint32_t index = 0;
    for (const float value : values)
    {
        if (index >= 3) break;
        request.values[index++] = value;
    }
    request.valueCount = index;
    return request;
}

TressfxSimulationConfig MakeConfig()
{
    TressfxSimulationConfig config;
    config.enabled = true;
    config.gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    config.damping = 0.035f;
    config.iterations = 8;
    config.bendStiffness = 0.3f;
    config.globalShapeStiffness = 0.0f;
    config.globalShapeEffectiveRange = 0.0f;
    config.vspCoefficient = 0.0f;
    config.vspAccelThresholdMax = 140.0f;
    return config;
}

// 逐字段快照用于断言“只改目标字段”。
struct Snapshot
{
    Eigen::Vector3f gravity;
    float damping = 0.0f;
    uint32_t iterations = 0;
    float bend = 0.0f;
    float globalShape = 0.0f;
    float globalShapeRange = 0.0f;
    float vsp = 0.0f;
    float vspMax = 0.0f;

    bool operator==(const Snapshot& other) const
    {
        return gravity.isApprox(other.gravity) &&
            damping == other.damping && iterations == other.iterations &&
            bend == other.bend && globalShape == other.globalShape &&
            globalShapeRange == other.globalShapeRange && vsp == other.vsp &&
            vspMax == other.vspMax;
    }
};

Snapshot Take(const TressfxSimulationConfig& config)
{
    Snapshot snapshot;
    snapshot.gravity = config.gravity;
    snapshot.damping = config.damping;
    snapshot.iterations = config.iterations;
    snapshot.bend = config.bendStiffness;
    snapshot.globalShape = config.globalShapeStiffness;
    snapshot.globalShapeRange = config.globalShapeEffectiveRange;
    snapshot.vsp = config.vspCoefficient;
    snapshot.vspMax = config.vspAccelThresholdMax;
    return snapshot;
}
} // namespace

TEST(TressfxLiveParams, SpecCoversEveryDocumentedLiveField)
{
    struct Case { const char* field; uint32_t count; };
    const Case cases[] = {
        {"gravity", 3}, {"damping", 1}, {"iterations", 1},
        {"bendStiffness", 1}, {"globalShapeStiffness", 1},
        {"globalShapeEffectiveRange", 1}, {"vspCoefficient", 1},
        {"vspAccelThresholdMax", 1},
    };
    for (const Case& item : cases)
    {
        const auto* spec = FindHairLiveParameterSpec(item.field);
        ASSERT_NE(spec, nullptr) << item.field;
        EXPECT_EQ(spec->componentCount, item.count) << item.field;
    }
    EXPECT_EQ(FindHairLiveParameterSpec("strandRadius"), nullptr);
    EXPECT_EQ(FindHairLiveParameterSpec("nonsense"), nullptr);
}

TEST(TressfxLiveParams, ReloadOnlyFieldsAreNamed)
{
    for (const char* field : {"enabled", "backend", "rootFixedPoints", "maxDt",
             "vspStiffness", "localShapeStiffness", "collision", "strandRadius",
             "skinning", "capsules", "boneWeights", "weights"})
        EXPECT_TRUE(IsHairReloadOnlyField(field)) << field;
    EXPECT_FALSE(IsHairReloadOnlyField("damping"));
}

TEST(TressfxLiveParams, ValuesAreRejectedOutOfRangeInsteadOfClamped)
{
    const auto* damping = FindHairLiveParameterSpec("damping");
    const auto* iterations = FindHairLiveParameterSpec("iterations");
    const auto* threshold = FindHairLiveParameterSpec("vspAccelThresholdMax");
    ASSERT_NE(damping, nullptr);
    ASSERT_NE(iterations, nullptr);
    ASSERT_NE(threshold, nullptr);
    const float badDampingHigh[] = {1.5f};
    const float badDampingLow[] = {-0.1f};
    const float goodDamping[] = {0.02f};
    EXPECT_NE(ValidateHairLiveParameterValues(*damping, badDampingHigh, 1), nullptr);
    EXPECT_NE(ValidateHairLiveParameterValues(*damping, badDampingLow, 1), nullptr);
    EXPECT_EQ(ValidateHairLiveParameterValues(*damping, goodDamping, 1), nullptr);

    const float zeroIterations[] = {0.0f};
    const float tooManyIterations[] = {17.0f};
    const float fractionalIterations[] = {3.5f};
    const float goodIterations[] = {8.0f};
    EXPECT_NE(ValidateHairLiveParameterValues(*iterations, zeroIterations, 1), nullptr);
    EXPECT_NE(ValidateHairLiveParameterValues(*iterations, tooManyIterations, 1), nullptr);
    EXPECT_NE(ValidateHairLiveParameterValues(*iterations, fractionalIterations, 1), nullptr);
    EXPECT_EQ(ValidateHairLiveParameterValues(*iterations, goodIterations, 1), nullptr);

    const float zeroThreshold[] = {0.0f};
    const float hugeThreshold[] = {2.0e7f};
    const float goodThreshold[] = {140.0f};
    EXPECT_NE(ValidateHairLiveParameterValues(*threshold, zeroThreshold, 1), nullptr);
    EXPECT_NE(ValidateHairLiveParameterValues(*threshold, hugeThreshold, 1), nullptr);
    EXPECT_EQ(ValidateHairLiveParameterValues(*threshold, goodThreshold, 1), nullptr);
}

TEST(TressfxLiveParams, ValuesRequireFiniteNumbersAndExactComponentCount)
{
    const auto* gravity = FindHairLiveParameterSpec("gravity");
    ASSERT_NE(gravity, nullptr);
    const float nanValues[] = {0.0f, std::nanf(""), 0.0f};
    const float infiniteValues[] = {0.0f, INFINITY, 0.0f};
    const float twoComponents[] = {1.0f, 2.0f};
    const float goodValues[] = {1.0f, 2.0f, 3.0f};
    EXPECT_NE(ValidateHairLiveParameterValues(*gravity, nanValues, 3), nullptr);
    EXPECT_NE(ValidateHairLiveParameterValues(*gravity, infiniteValues, 3), nullptr);
    EXPECT_NE(ValidateHairLiveParameterValues(*gravity, twoComponents, 2), nullptr);
    EXPECT_EQ(ValidateHairLiveParameterValues(*gravity, goodValues, 3), nullptr);
}

TEST(TressfxLiveParams, ApplyChangesOnlyTheTargetField)
{
    TressfxSimulationConfig config = MakeConfig();
    const Snapshot before = Take(config);
    std::string error;
    ASSERT_TRUE(ApplyHairLiveParameter(config,
        MakeRequest("hair", "damping", {0.02f}), error)) << error;
    Snapshot expected = before;
    expected.damping = 0.02f;
    EXPECT_TRUE(Take(config) == expected);
}

TEST(TressfxLiveParams, ApplyWritesVectorsAndIntegerIterations)
{
    TressfxSimulationConfig config = MakeConfig();
    std::string error;
    ASSERT_TRUE(ApplyHairLiveParameter(config,
        MakeRequest("hair", "gravity", {0.0f, -500.0f, 25.0f}), error)) << error;
    EXPECT_TRUE(config.gravity.isApprox(Eigen::Vector3f(0.0f, -500.0f, 25.0f)));
    ASSERT_TRUE(ApplyHairLiveParameter(config,
        MakeRequest("hair", "iterations", {12.0f}), error)) << error;
    EXPECT_EQ(config.iterations, 12u);
}

TEST(TressfxLiveParams, ApplyRejectsInvalidValuesWithoutPartialWrite)
{
    TressfxSimulationConfig config = MakeConfig();
    const Snapshot before = Take(config);
    std::string error;
    EXPECT_FALSE(ApplyHairLiveParameter(config,
        MakeRequest("hair", "damping", {2.0f}), error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(Take(config) == before);
    EXPECT_FALSE(ApplyHairLiveParameter(config,
        MakeRequest("hair", "rootFixedPoints", {4.0f}), error));
    EXPECT_NE(error.find("world reload"), std::string::npos);
    EXPECT_TRUE(Take(config) == before);
    EXPECT_FALSE(ApplyHairLiveParameter(config,
        MakeRequest("hair", "unknownField", {1.0f}), error));
    EXPECT_TRUE(Take(config) == before);
}

TEST(TressfxLiveParams, RequestValidationNamesTheProblem)
{
    HairLiveParameterRequest request = MakeRequest("hair", "damping", {0.02f});
    std::string error;
    EXPECT_TRUE(ValidateHairLiveParameterRequest(request, error));
    request = MakeRequest("hair", "strandRadius", {0.01f});
    EXPECT_FALSE(ValidateHairLiveParameterRequest(request, error));
    EXPECT_NE(error.find("world reload"), std::string::npos);
    // `hair set` 没有“全部对象”语义：目标名必须明确。
    request = MakeRequest("", "damping", {0.02f});
    EXPECT_FALSE(ValidateHairLiveParameterRequest(request, error));
    EXPECT_NE(error.find("object name"), std::string::npos);
}

TEST(TressfxLiveParams, ParseAcceptsScalarAndVectorFields)
{
    HairLiveParameterRequest request;
    ASSERT_EQ(ParseHairSetArguments(" preview_model damping 0.02 ", request), nullptr);
    EXPECT_EQ(request.objectName, "preview_model");
    EXPECT_EQ(request.field, "damping");
    ASSERT_EQ(request.valueCount, 1u);
    EXPECT_FLOAT_EQ(request.values[0], 0.02f);

    ASSERT_EQ(ParseHairSetArguments("hair gravity -100 0 50", request), nullptr);
    EXPECT_EQ(request.objectName, "hair");
    EXPECT_EQ(request.field, "gravity");
    ASSERT_EQ(request.valueCount, 3u);
    EXPECT_FLOAT_EQ(request.values[0], -100.0f);
    EXPECT_FLOAT_EQ(request.values[2], 50.0f);
}

TEST(TressfxLiveParams, ParseRejectsMalformedArguments)
{
    HairLiveParameterRequest request;
    EXPECT_NE(ParseHairSetArguments("", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("onlyObject", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj damping", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj damping 0.02 extra", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj gravity 1 2", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj gravity 1 2 3 4", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj damping 0.02x", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj damping nan", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj damping 1.5", request), nullptr);
    EXPECT_NE(ParseHairSetArguments("obj notAField 1", request), nullptr);
    // 需要重载的字段给出专门原因，而不是笼统的 unknown。
    const char* reload = ParseHairSetArguments("obj skinning 1", request);
    ASSERT_NE(reload, nullptr);
    EXPECT_NE(std::string(reload).find("world reload"), std::string::npos);
    // 失败不发布半解析结果。
    EXPECT_TRUE(request.objectName.empty());
    EXPECT_TRUE(request.field.empty());
    EXPECT_EQ(request.valueCount, 0u);
}

TEST(TressfxLiveParams, PlacementTargetChangesOnlyOneInstanceOfSharedHair)
{
    VL::Hair::TressfxSimulationOwner firstOwner;
    VL::Hair::TressfxSimulationOwner secondOwner;
    firstOwner.Observe("preview_model");
    firstOwner.Observe("preview_model"); // 同 placement 的重复材质 draw。
    secondOwner.Observe("another_hair");
    TressfxSimulationConfig configs[] = {MakeConfig(), MakeConfig()};
    const VL::Hair::TressfxSimulationOwner owners[] = {firstOwner, secondOwner};
    const HairLiveParameterRequest request =
        MakeRequest("preview_model", "damping", {0.5f});
    std::string error;
    for (size_t index = 0; index < std::size(configs); ++index)
        if (owners[index].MatchesCommandTarget(request.objectName))
            ASSERT_TRUE(ApplyHairLiveParameter(configs[index], request, error)) << error;
    EXPECT_FLOAT_EQ(configs[0].damping, 0.5f);
    EXPECT_FLOAT_EQ(configs[1].damping, 0.035f);
    EXPECT_FALSE(firstOwner.MatchesCommandTarget(
        "female_guides.tfx|behavior={}|instance=preview_model"));
    EXPECT_FALSE(firstOwner.MatchesCommandTarget("preview"));
    EXPECT_FALSE(firstOwner.MatchesCommandTarget(""));
}

TEST(TressfxLiveParams, NamedResetIsPlacementScopedAndEmptyResetIncludesAll)
{
    VL::Hair::TressfxSimulationOwner firstOwner;
    VL::Hair::TressfxSimulationOwner secondOwner;
    firstOwner.Observe("first");
    secondOwner.Observe("second");
    EXPECT_TRUE(firstOwner.MatchesCommandTarget("first", true));
    EXPECT_FALSE(secondOwner.MatchesCommandTarget("first", true));
    EXPECT_TRUE(firstOwner.MatchesCommandTarget("", true));
    EXPECT_TRUE(secondOwner.MatchesCommandTarget("", true));
}

TEST(TressfxLiveParams, ConflictingOrUnobservedPlacementCannotReceiveCommands)
{
    VL::Hair::TressfxSimulationOwner owner;
    EXPECT_FALSE(owner.MatchesCommandTarget("first"));
    EXPECT_FALSE(owner.MatchesCommandTarget("", true));
    owner.Observe("first");
    owner.Observe("second");
    EXPECT_FALSE(owner.MatchesCommandTarget("first"));
    EXPECT_FALSE(owner.MatchesCommandTarget("second"));
    EXPECT_FALSE(owner.MatchesCommandTarget("", true));
}
