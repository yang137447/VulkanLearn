#include "render/resource/terrainCompositedQualityData.h"

#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace
{
namespace Abi = VL::TerrainQualityAbi;

class QualityBinary : public testing::Test
{
protected:
    std::vector<uint32_t> words;

    void SetUp() override
    {
        words.resize(Abi::Bytes / 4);
        words[Abi::MagicWord] = Abi::Magic;
        words[Abi::VersionWord] = Abi::Version;
        words[Abi::CapacityWord] = Abi::Capacity;
        words[Abi::VisibleCount] = words[Abi::ResolvedCount] = 1;
        words[Abi::EpochLow] = 20;
        words[Abi::GenerationLow] = 19;
        words[Abi::RenderWidth] = 1920;
        words[Abi::RenderHeight] = 1080;
        words[Abi::SurfaceCount] = 1;
        Abi::Sample sample{{.5f, .5f}, {.01f, 0}, 2.25f, 0, 8u << 16, 1};
        std::memcpy(words.data() + Abi::SampleWord, &sample, sizeof(sample));
        Abi::Surface surface{0, 8192, 8192, 13, 19, 0, 7, 0, {0, 1, 2, 3, 4, 5, 6, 7}};
        std::memcpy(words.data() + Abi::SurfaceWord, &surface, sizeof(surface));
        for (uint32_t branch = 0; branch < 2; ++branch)
        {
            Abi::Owner owner{1, 0, 2 + branch, 0, 19, 0, 7, 0, surface.identity};
            std::memcpy(words.data() + Abi::OwnerWord + branch * 16, &owner, sizeof(owner));
            for (uint32_t tap = 0; tap < 8; ++tap)
                words[Abi::ResultWord + tap * 2 + branch] =
                    branch + 1 | ((2 + branch) << 10) | (1u << 26);
        }
    }

    Abi::Counts Counts() { return Abi::Recompute(words.data(), words.size() * 4); }
};

TEST_F(QualityBinary, AllEightTapPairsAreRequired)
{
    EXPECT_TRUE(Counts().MeetsQualityThreshold());
    words[Abi::ResultWord + 15] = 0;
    EXPECT_FALSE(Counts().complete);
    EXPECT_FALSE(Counts().MeetsQualityThreshold());
}

TEST_F(QualityBinary, MissingEntryAndUnknownOwnerStayInDenominator)
{
    words[Abi::ResultWord] = (13u << 10) | (1u << 26);
    EXPECT_EQ(Counts().visibleSamples, 1u);
    EXPECT_EQ(Counts().unresolved, 1u);
    EXPECT_FALSE(Counts().MeetsQualityThreshold());
    words[Abi::ResultWord] = 1 | (2u << 10) | (1u << 26);
    words[Abi::OwnerWord] = 0;
    EXPECT_EQ(Counts().unresolved, 1u);
}

TEST_F(QualityBinary, InvalidSlotCannotIndexOwnerDictionary)
{
    words[Abi::ResultWord] = 513 | (2u << 10) | (1u << 26);
    EXPECT_EQ(Counts().unresolved, 1u);
    EXPECT_FALSE(Counts().complete);
    EXPECT_FALSE(Counts().MeetsQualityThreshold());
}

TEST_F(QualityBinary, InvalidOwnerStateIsIncomplete)
{
    words[Abi::OwnerWord] = 2;
    EXPECT_FALSE(Counts().complete);
    EXPECT_FALSE(Counts().MeetsQualityThreshold());
}

TEST_F(QualityBinary, OwnerIdentityRevisionGenerationAndKeyAreExact)
{
    for (uint32_t offset : {1u, 2u, 3u, 4u, 6u, 8u})
    {
        const auto original = words[Abi::OwnerWord + offset];
        words[Abi::OwnerWord + offset] = 999;
        EXPECT_EQ(Counts().incorrectIdentity, 1u);
        EXPECT_FALSE(Counts().MeetsQualityThreshold());
        words[Abi::OwnerWord + offset] = original;
    }
}

TEST_F(QualityBinary, DuplicatesInvalidDropsAndOverflowAreIncomplete)
{
    for (uint32_t offset : {uint32_t(Abi::DuplicateCount), uint32_t(Abi::InvalidCount),
        uint32_t(Abi::DroppedTerrainSamples), uint32_t(Abi::OverflowCount)})
    {
        words[offset] = 1;
        EXPECT_FALSE(Counts().complete);
        words[offset] = 0;
    }
    words[Abi::SampleWord + 7] = 2;
    EXPECT_FALSE(Counts().complete);
}

TEST_F(QualityBinary, MissingSampleAndResolutionCountersAreIncomplete)
{
    words[Abi::ResolvedCount] = 0;
    EXPECT_FALSE(Counts().complete);
    words[Abi::ResolvedCount] = 1;
    words[Abi::SampleWord + 7] = 0;
    EXPECT_FALSE(Counts().complete);
}

TEST_F(QualityBinary, InvalidBiasTargetTapCountAndReservedBitsAreIncomplete)
{
    const auto originalTarget = words[Abi::SampleWord + 4];
    for (float value : {-1.f, 14.f, std::numeric_limits<float>::quiet_NaN()})
    {
        std::memcpy(words.data() + Abi::SampleWord + 4, &value, 4);
        EXPECT_FALSE(Counts().complete);
    }
    words[Abi::SampleWord + 4] = originalTarget;
    for (float value : {-1.f, .5f, 2.f, std::numeric_limits<float>::infinity()})
    {
        std::memcpy(words.data() + Abi::SampleWord + 5, &value, 4);
        EXPECT_FALSE(Counts().complete);
    }
    words[Abi::SampleWord + 5] = 0;
    for (uint32_t taps : {0u, 9u})
    {
        words[Abi::SampleWord + 6] = taps << 16;
        EXPECT_FALSE(Counts().complete);
    }
    words[Abi::SampleWord + 6] = 8u << 16;
    words[Abi::ResultWord] |= 1u << 31;
    EXPECT_FALSE(Counts().complete);
}

TEST_F(QualityBinary, BiasDoesNotRelaxOriginalTargetPlusOne)
{
    float bias = 1;
    std::memcpy(words.data() + Abi::SampleWord + 5, &bias, 4);
    for (uint32_t branch = 0; branch < 2; ++branch)
    {
        words[Abi::OwnerWord + branch * 16 + 2] = 4 + branch;
        for (uint32_t tap = 0; tap < 8; ++tap)
            words[Abi::ResultWord + tap * 2 + branch] =
                branch + 1 | ((4 + branch) << 10) | (1u << 26);
    }
    EXPECT_TRUE(Counts().complete);
    EXPECT_EQ(Counts().effectiveMipBias, 1u);
    EXPECT_EQ(Counts().validAtTargetPlusBias, 0u);
}

TEST_F(QualityBinary, EpochAndSurfaceGenerationCannotBeMissing)
{
    words[Abi::EpochLow] = 0;
    EXPECT_THROW(Counts(), std::runtime_error);
    words[Abi::EpochLow] = 20;
    words[Abi::GenerationLow] = 21;
    EXPECT_FALSE(Counts().complete);
}

TEST_F(QualityBinary, RawTargetZeroRetainsFineZeroAndRejectsCoarseTwo)
{
    const float target = .25f;
    std::memcpy(words.data() + Abi::SampleWord + 4, &target, 4);
    for (uint32_t branch = 0; branch < 2; ++branch)
    {
        words[Abi::OwnerWord + branch * 16 + 2] = branch;
        for (uint32_t tap = 0; tap < 8; ++tap)
            words[Abi::ResultWord + tap * 2 + branch] =
                branch + 1 | (branch << 10) | (1u << 26);
    }
    EXPECT_EQ(Counts().effectiveMipBias, 0u);
    EXPECT_TRUE(Counts().MeetsQualityThreshold());
    words[Abi::OwnerWord + 2] = 2;
    for (uint32_t tap = 0; tap < 8; ++tap)
        words[Abi::ResultWord + tap * 2] = 1 | (2u << 10) | (1u << 26);
    EXPECT_FALSE(Counts().MeetsQualityThreshold());
}

TEST_F(QualityBinary, RootOnlyAndUninitializedCannotReleaseLoading)
{
    for (uint32_t branch = 0; branch < 2; ++branch)
    {
        words[Abi::OwnerWord + branch * 16 + 2] = 13;
        for (uint32_t tap = 0; tap < 8; ++tap)
            words[Abi::ResultWord + tap * 2 + branch] =
                branch + 1 | (13u << 10) | (1u << 26);
    }
    EXPECT_EQ(Counts().rootOnlySamples, 1u);
    EXPECT_FALSE(Counts().MeetsQualityThreshold());
    words[Abi::UninitializedDraws] = 1;
    EXPECT_EQ(Counts().uninitializedDraws, 1u);
}

TEST_F(QualityBinary, ThresholdIsExactlyNinetyNinePercent)
{
    Abi::Counts counts;
    counts.complete = true;
    counts.visibleSamples = 100;
    counts.validAtTargetPlusBias = 99;
    EXPECT_TRUE(counts.MeetsQualityThreshold());
    counts.validAtTargetPlusBias = 98;
    EXPECT_FALSE(counts.MeetsQualityThreshold());
}
}
