#include <array>
#include <cmath>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleAsset.h"
#include "particles/particleCurveEdit.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;
using namespace VL::ParticleCurveEdit;

Json ColorKey(float time, std::array<float, 4> color)
{
    return Json{{"time", time},
        {"color", Json::array({color[0], color[1], color[2], color[3]})}};
}

Json SizeKey(float time, float size)
{
    return Json{{"time", time}, {"size", size}};
}

Json MinimalDocument()
{
    return Json{
        {"name", "PS_curve_edit"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "spark"},
                {"lifetime", {1.0, 1.0}},
                {"colorStart", {1.0, 0.5, 0.25, 1.0}},
                {"colorEnd", {0.0, 0.0, 0.0, 0.0}},
                {"size", {10.0, 2.0}}}
        })}};
}

} // namespace

TEST(ParticleCurveEdit, MissingColorKeysComeFromEndpointColors)
{
    const Json emitter{
        {"colorStart", {1.0, 0.5, 0.25}},
        {"colorEnd", {0.0, 0.0, 0.0, 0.5}}};
    const Json keys = ReadColorKeys(emitter);

    ASSERT_EQ(keys.size(), 2u);
    EXPECT_FLOAT_EQ(keys.at(0).at("time").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(0).get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(3).get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(keys.at(1).at("time").get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(keys.at(1).at("color").at(3).get<float>(), 0.5f);
}

TEST(ParticleCurveEdit, ExistingColorKeysKeepHdrRgbAndSort)
{
    const Json emitter{
        {"colorKeys", Json::array({
            ColorKey(0.75f, {0.0f, 0.5f, 1.0f, 1.0f}),
            Json{{"time", 0.25f},
                {"color", Json::array({2.0f, -1.0f, 0.5f})}}
        })}};
    const Json keys = ReadColorKeys(emitter);

    ASSERT_EQ(keys.size(), 2u);
    EXPECT_FLOAT_EQ(keys.at(0).at("time").get<float>(), 0.25f);
    // RGB 是线性 HDR 强度，编辑层不能把 2.0 压成 1.0；负数仍按 0。
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(0).get<float>(), 2.0f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(1).get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(3).get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(keys.at(1).at("time").get<float>(), 0.75f);
}

TEST(ParticleCurveEdit, HdrRgbRoundTripsWhileAlphaClamps)
{
    const Json emitter{
        {"colorKeys", Json::array({
            ColorKey(0.0f, {4.5f, 2.0f, 0.25f, 1.0f})
        })}};
    Json keys = ReadColorKeys(emitter);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(0).get<float>(), 4.5f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(1).get<float>(), 2.0f);

    SetColorComponent(keys, 0, 0, 6.0f);
    SetColorComponent(keys, 0, 3, 1.7f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(0).get<float>(), 6.0f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(3).get<float>(), 1.0f);
    SetColorComponent(keys, 0, 3, -0.2f);
    EXPECT_FLOAT_EQ(keys.at(0).at("color").at(3).get<float>(), 0.0f);

    Json document = MinimalDocument();
    document["emitters"][0]["colorKeys"] = keys;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_hdr_curve.json");
    ASSERT_EQ(asset.emitters[0].colorKeys.size(), 1u);
    EXPECT_FLOAT_EQ(asset.emitters[0].colorKeys[0].color.x(), 6.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].colorKeys[0].color.y(), 2.0f);

    // Hex 只用于 UI 预览，显示时仍按 [0,1] 量化。
    EXPECT_EQ(
        ColorHex({6.0f, 0.5f, 0.25f, 1.0f}),
        "#FF8040FF");
}

TEST(ParticleCurveEdit, MissingSizeKeysComeFromEndpointSizes)
{
    const Json emitter{{"size", {12.0, 3.0}}};
    const Json keys = ReadSizeKeys(emitter);

    ASSERT_EQ(keys.size(), 2u);
    EXPECT_FLOAT_EQ(keys.at(0).at("time").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(keys.at(0).at("size").get<float>(), 12.0f);
    EXPECT_FLOAT_EQ(keys.at(1).at("size").get<float>(), 3.0f);
}

TEST(ParticleCurveEdit, ExistingSizeKeysAreClampedAndSorted)
{
    const Json emitter{
        {"sizeKeys", Json::array({
            SizeKey(0.8f, 5.0f),
            SizeKey(0.2f, -3.0f)
        })}};
    const Json keys = ReadSizeKeys(emitter);

    ASSERT_EQ(keys.size(), 2u);
    EXPECT_FLOAT_EQ(keys.at(0).at("time").get<float>(), 0.2f);
    EXPECT_FLOAT_EQ(keys.at(0).at("size").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(keys.at(1).at("time").get<float>(), 0.8f);
}

TEST(ParticleCurveEdit, AddKeyUsesLargestGapAndInterpolates)
{
    Json colorKeys = Json::array({
        ColorKey(0.0f, {0.0f, 0.0f, 0.0f, 1.0f}),
        ColorKey(0.2f, {1.0f, 1.0f, 1.0f, 1.0f}),
        ColorKey(1.0f, {0.0f, 0.0f, 0.0f, 1.0f})});
    ASSERT_TRUE(AddKey(colorKeys, true));
    ASSERT_EQ(colorKeys.size(), 4u);
    EXPECT_NEAR(colorKeys.at(2).at("time").get<float>(), 0.6f, 1e-5f);
    EXPECT_NEAR(colorKeys.at(2).at("color").at(0).get<float>(), 0.5f, 1e-5f);

    Json sizeKeys = Json::array({
        SizeKey(0.0f, 10.0f),
        SizeKey(0.2f, 20.0f),
        SizeKey(1.0f, 0.0f)});
    ASSERT_TRUE(AddKey(sizeKeys, false));
    ASSERT_EQ(sizeKeys.size(), 4u);
    EXPECT_NEAR(sizeKeys.at(2).at("time").get<float>(), 0.6f, 1e-5f);
    EXPECT_NEAR(sizeKeys.at(2).at("size").get<float>(), 10.0f, 1e-5f);
}

TEST(ParticleCurveEdit, AddKeyRejectsTheNinthKey)
{
    Json keys = Json::array();
    for (size_t index = 0; index < kMaxKeys; ++index)
        keys.push_back(SizeKey(static_cast<float>(index) /
            static_cast<float>(kMaxKeys), 1.0f));
    ASSERT_EQ(keys.size(), kMaxKeys);
    const Json before = keys;

    EXPECT_FALSE(AddKey(keys, false));
    EXPECT_EQ(keys, before);
}

TEST(ParticleCurveEdit, SetKeyTimeClampsAndRejectsCrossingNeighbours)
{
    Json keys = Json::array({
        SizeKey(0.0f, 1.0f),
        SizeKey(0.5f, 2.0f),
        SizeKey(1.0f, 3.0f)});

    ASSERT_TRUE(SetKeyTime(keys, 2, 1.5f));
    EXPECT_FLOAT_EQ(keys.at(2).at("time").get<float>(), 1.0f);

    const Json before = keys;
    // 时间 1 与右侧 1.0 重叠，0.9995 又小于 0.001 间隔，两者都必须拒绝。
    EXPECT_FALSE(SetKeyTime(keys, 1, 1.0f));
    EXPECT_EQ(keys, before);
    EXPECT_FALSE(SetKeyTime(keys, 1, 0.9995f));
    EXPECT_EQ(keys, before);

    ASSERT_TRUE(SetKeyTime(keys, 1, 0.4f));
    EXPECT_FLOAT_EQ(keys.at(1).at("time").get<float>(), 0.4f);
    const Json afterMove = keys;
    EXPECT_FALSE(SetKeyTime(keys, 0, 0.4f));
    EXPECT_EQ(keys, afterMove);
}

TEST(ParticleCurveEdit, RemoveKeyKeepsAtLeastOneKey)
{
    Json oneKey = Json::array({SizeKey(0.5f, 2.0f)});
    EXPECT_FALSE(RemoveKey(oneKey, 0));
    ASSERT_EQ(oneKey.size(), 1u);

    Json twoKeys = Json::array({
        SizeKey(0.0f, 1.0f), SizeKey(1.0f, 2.0f)});
    EXPECT_TRUE(RemoveKey(twoKeys, 0));
    ASSERT_EQ(twoKeys.size(), 1u);
    EXPECT_FLOAT_EQ(twoKeys.at(0).at("time").get<float>(), 1.0f);
}

TEST(ParticleCurveEdit, RemovingExplicitCurveFieldsRestoresEndpointKeys)
{
    Json emitter{
        {"colorStart", {1.0, 0.25, 0.0, 1.0}},
        {"colorEnd", {0.0, 0.0, 1.0, 0.0}},
        {"colorKeys", Json::array({
            ColorKey(0.5f, {1.0f, 1.0f, 1.0f, 1.0f})})},
        {"size", {12.0, 3.0}},
        {"sizeKeys", Json::array({SizeKey(0.5f, 7.0f)})}};
    ASSERT_EQ(ReadColorKeys(emitter).size(), 1u);
    ASSERT_EQ(ReadSizeKeys(emitter).size(), 1u);

    // 面板删除单 key 曲线时提交 reset 移除字段；预览随后必须回到端点值。
    emitter.erase("colorKeys");
    emitter.erase("sizeKeys");
    const Json colors = ReadColorKeys(emitter);
    const Json sizes = ReadSizeKeys(emitter);
    ASSERT_EQ(colors.size(), 2u);
    ASSERT_EQ(sizes.size(), 2u);
    EXPECT_FLOAT_EQ(colors.at(0).at("color").at(1).get<float>(), 0.25f);
    EXPECT_FLOAT_EQ(colors.at(1).at("color").at(2).get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(sizes.at(0).at("size").get<float>(), 12.0f);
    EXPECT_FLOAT_EQ(sizes.at(1).at("size").get<float>(), 3.0f);
}

TEST(ParticleCurveEdit, SamplingClampsOutsideAndInterpolatesInside)
{
    const Json colorKeys = Json::array({
        ColorKey(0.0f, {0.0f, 0.0f, 0.0f, 1.0f}),
        ColorKey(0.5f, {0.5f, 0.5f, 0.5f, 1.0f}),
        ColorKey(1.0f, {1.0f, 1.0f, 1.0f, 0.0f})});

    const auto before = SampleColor(colorKeys, -1.0f);
    EXPECT_FLOAT_EQ(before[0], 0.0f);
    const auto after = SampleColor(colorKeys, 2.0f);
    EXPECT_FLOAT_EQ(after[3], 0.0f);
    const auto middle = SampleColor(colorKeys, 0.75f);
    EXPECT_NEAR(middle[0], 0.75f, 1e-5f);
    EXPECT_NEAR(middle[3], 0.5f, 1e-5f);

    const Json sizeKeys = Json::array({
        SizeKey(0.0f, 10.0f),
        SizeKey(0.4f, 20.0f),
        SizeKey(1.0f, 0.0f)});
    EXPECT_FLOAT_EQ(SampleSize(sizeKeys, -1.0f), 10.0f);
    EXPECT_FLOAT_EQ(SampleSize(sizeKeys, 2.0f), 0.0f);
    EXPECT_NEAR(SampleSize(sizeKeys, 0.2f), 15.0f, 1e-5f);
}

TEST(ParticleCurveEdit, ColorHexUsesUppercaseRgba)
{
    EXPECT_EQ(ColorHex({1.0f, 0.5f, 0.0f, 1.0f}), "#FF8000FF");
    EXPECT_EQ(ColorHex({0.0f, 0.0f, 0.0f, 0.0f}), "#00000000");
    EXPECT_EQ(ColorHex({2.0f, -1.0f, 1.0f, 0.5f}), "#FF00FF80");
}

TEST(ParticleCurveEdit, EditedColorAndSizeCurvesRoundTripThroughAssetValidation)
{
    Json document = MinimalDocument();
    Json& emitter = document["emitters"][0];

    Json colorKeys = ReadColorKeys(emitter);
    ASSERT_TRUE(AddKey(colorKeys, true));
    ASSERT_TRUE(SetKeyTime(colorKeys, 1, 0.4f));
    SetColorComponent(colorKeys, 1, 0, 0.25f);
    SetColorComponent(colorKeys, 1, 3, 0.5f);
    emitter["colorKeys"] = colorKeys;

    Json sizeKeys = ReadSizeKeys(emitter);
    ASSERT_TRUE(AddKey(sizeKeys, false));
    SetSize(sizeKeys, 1, 7.5f);
    emitter["sizeKeys"] = sizeKeys;

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_curve_edit.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    ASSERT_EQ(asset.emitters[0].colorKeys.size(), 3u);
    ASSERT_EQ(asset.emitters[0].sizeKeys.size(), 3u);
    EXPECT_NEAR(asset.emitters[0].colorKeys[1].time, 0.4f, 1e-5f);
    EXPECT_NEAR(asset.emitters[0].colorKeys[1].color.x(), 0.25f, 1e-5f);
    EXPECT_NEAR(asset.emitters[0].colorKeys[1].color.w(), 0.5f, 1e-5f);
    EXPECT_NEAR(asset.emitters[0].sizeKeys[1].size, 7.5f, 1e-5f);
}
