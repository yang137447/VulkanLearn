#include "hair/tfxBoneLoader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
using VL::Hair::MapTfxBoneToSkeleton;
using VL::Hair::ParseTfxBone;
using VL::Hair::TfxBoneAsset;

void AppendI32(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFFu));
}

void AppendFloat(std::vector<std::uint8_t>& bytes, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendI32(bytes, static_cast<std::int32_t>(bits));
}

void AppendBoneName(std::vector<std::uint8_t>& bytes, std::int32_t index,
    const std::string& name)
{
    // 源 LoadBoneData 的 charLen 包含结尾 null。
    AppendI32(bytes, index);
    AppendI32(bytes, static_cast<std::int32_t>(name.size() + 1));
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back(0);
}

std::vector<std::uint8_t> BasicBoneFile()
{
    std::vector<std::uint8_t> bytes;
    AppendI32(bytes, 2); // numBones
    AppendBoneName(bytes, 0, "Head_M");
    AppendBoneName(bytes, 1, "Neck1_M");
    AppendI32(bytes, 2); // numStrands
    // strand 0: 75% Head + 25% Neck
    AppendI32(bytes, 0);
    AppendI32(bytes, 2);
    AppendI32(bytes, 0);
    AppendFloat(bytes, 0.75f);
    AppendI32(bytes, 1);
    AppendFloat(bytes, 0.25f);
    // strand 1: 100% Neck
    AppendI32(bytes, 1);
    AppendI32(bytes, 1);
    AppendI32(bytes, 1);
    AppendFloat(bytes, 1.0f);
    return bytes;
}

VL::RuntimeResult<TfxBoneAsset> Parse(const std::vector<std::uint8_t>& bytes)
{
    return ParseTfxBone(bytes, "SM_test.tfxbone");
}
} // namespace

TEST(TfxBoneLoader, ParsesBoneNamesAndStrandWeights)
{
    const auto parsed = Parse(BasicBoneFile());
    ASSERT_TRUE(parsed.IsSuccess()) << VL::FormatRuntimeError(parsed.Error());
    const TfxBoneAsset& asset = parsed.Value();
    ASSERT_EQ(asset.boneNames.size(), 2u);
    EXPECT_EQ(asset.boneNames[0], "Head_M");
    EXPECT_EQ(asset.boneNames[1], "Neck1_M");
    ASSERT_EQ(asset.strands.size(), 2u);
    ASSERT_EQ(asset.strands[0].influences.size(), 2u);
    EXPECT_EQ(asset.strands[0].influences[0].boneIndex, 0);
    EXPECT_FLOAT_EQ(asset.strands[0].influences[0].weight, 0.75f);
    EXPECT_EQ(asset.strands[1].influences[0].boneIndex, 1);
}

TEST(TfxBoneLoader, RejectsTruncatedBoneTable)
{
    auto bytes = BasicBoneFile();
    bytes.resize(bytes.size() - 3);
    const auto parsed = Parse(bytes);
    ASSERT_FALSE(parsed.IsSuccess());
    EXPECT_EQ(parsed.Error().code, "Hair.TfxBoneTruncated");
    EXPECT_EQ(parsed.Error().sourcePath, "SM_test.tfxbone");
}

TEST(TfxBoneLoader, RejectsOutOfRangeBoneIndexAndInvalidWeight)
{
    auto badIndex = BasicBoneFile();
    // strand 1 的第一条影响：boneIndex 位于文件末尾前 8 字节。
    badIndex[badIndex.size() - 8] = 9;
    const auto indexResult = Parse(badIndex);
    ASSERT_FALSE(indexResult.IsSuccess());
    EXPECT_EQ(indexResult.Error().code, "Hair.TfxBoneIndex");

    auto badWeight = BasicBoneFile();
    // 把 strand 1 的 1.0f 改成 NaN。
    const std::uint32_t nanBits = 0x7FC00000u;
    for (int byte = 0; byte < 4; ++byte)
        badWeight[badWeight.size() - 4 + byte] =
            static_cast<std::uint8_t>((nanBits >> (byte * 8)) & 0xFFu);
    const auto weightResult = Parse(badWeight);
    ASSERT_FALSE(weightResult.IsSuccess());
    EXPECT_EQ(weightResult.Error().code, "Hair.TfxBoneWeight");
}

TEST(TfxBoneLoader, RejectsMoreStrandsThanGuides)
{
    const auto parsed = Parse(BasicBoneFile());
    ASSERT_TRUE(parsed.IsSuccess());
    std::vector<std::string> joints{"Head_M", "Neck1_M"};
    std::vector<std::string> unmapped;
    const auto mapped = MapTfxBoneToSkeleton(
        parsed.Value(), joints, 1, unmapped, "SM_test.tfxbone");
    ASSERT_FALSE(mapped.IsSuccess());
    EXPECT_EQ(mapped.Error().code, "Hair.TfxBoneGuideMismatch");
}

TEST(TfxBoneLoader, MapsNamesToJointsAndNormalizesWeights)
{
    const auto parsed = Parse(BasicBoneFile());
    ASSERT_TRUE(parsed.IsSuccess());
    const std::vector<std::string> joints{"Root", "Head_M", "Neck1_M"};
    std::vector<std::string> unmapped;
    const auto mapped = MapTfxBoneToSkeleton(
        parsed.Value(), joints, 4, unmapped, "SM_test.tfxbone");
    ASSERT_TRUE(mapped.IsSuccess()) << VL::FormatRuntimeError(mapped.Error());
    const auto& strands = mapped.Value();
    ASSERT_EQ(strands.size(), 4u);
    ASSERT_EQ(strands[0].joints.size(), 2u);
    EXPECT_EQ(strands[0].joints[0], 1u);
    EXPECT_EQ(strands[0].joints[1], 2u);
    EXPECT_NEAR(strands[0].weights[0], 0.75f, 1.0e-6f);
    EXPECT_NEAR(strands[0].weights[1], 0.25f, 1.0e-6f);
    // 文件只有 2 根 strand；其余 guide 必须为空权重，调用方回退根骨。
    EXPECT_TRUE(strands[2].joints.empty());
    EXPECT_TRUE(strands[3].weights.empty());
    EXPECT_TRUE(unmapped.empty());
}

TEST(TfxBoneLoader, SkipsNegativeAndZeroInfluencesLikeSource)
{
    std::vector<std::uint8_t> bytes;
    AppendI32(bytes, 1);
    AppendBoneName(bytes, 0, "Head_M");
    AppendI32(bytes, 1);
    AppendI32(bytes, 0);
    AppendI32(bytes, 3);
    AppendI32(bytes, -1); // 源 INDEX_NONE，跳过
    AppendFloat(bytes, 1.0f);
    AppendI32(bytes, 0); // weight 0，跳过
    AppendFloat(bytes, 0.0f);
    AppendI32(bytes, 0);
    AppendFloat(bytes, 2.0f); // 只剩这一条，归一化后为 1
    const auto parsed = Parse(bytes);
    ASSERT_TRUE(parsed.IsSuccess());
    const std::vector<std::string> joints{"Head_M"};
    std::vector<std::string> unmapped;
    const auto mapped = MapTfxBoneToSkeleton(parsed.Value(), joints, 1, unmapped);
    ASSERT_TRUE(mapped.IsSuccess());
    ASSERT_EQ(mapped.Value()[0].joints.size(), 1u);
    EXPECT_NEAR(mapped.Value()[0].weights[0], 1.0f, 1.0e-6f);
}

TEST(TfxBoneLoader, UnmappedBoneIsSkippedAndReported)
{
    const auto parsed = Parse(BasicBoneFile());
    ASSERT_TRUE(parsed.IsSuccess());
    const std::vector<std::string> joints{"Head_M"}; // 缺 Neck1_M
    std::vector<std::string> unmapped;
    const auto mapped = MapTfxBoneToSkeleton(
        parsed.Value(), joints, 2, unmapped, "SM_test.tfxbone");
    ASSERT_TRUE(mapped.IsSuccess());
    ASSERT_EQ(unmapped.size(), 1u);
    EXPECT_EQ(unmapped[0], "Neck1_M");
    // strand 0 只剩 Head_M，权重归一化为 1；strand 1 没有任何已映射骨。
    ASSERT_EQ(mapped.Value()[0].joints.size(), 1u);
    EXPECT_NEAR(mapped.Value()[0].weights[0], 1.0f, 1.0e-6f);
    EXPECT_TRUE(mapped.Value()[1].joints.empty());
}
