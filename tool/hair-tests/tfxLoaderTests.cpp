#include "hair/tfxLoader.h"
#include "tfxFixture.h"

#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <limits>

namespace
{
using namespace VL::Hair;
using namespace TfxFixture;

void ExpectError(const std::vector<std::uint8_t>& bytes, const char* code, const char* field)
{
    const auto result = ParseTfx(bytes, "fixture.tfx");
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, code);
    EXPECT_EQ(result.Error().sourcePath, "fixture.tfx");
    EXPECT_EQ(result.Error().sourceNode, field);
}

TEST(TfxLoader, PreservesGuideCountWithoutComputePadding)
{
    for (const auto guides : {1u, 3u, 64u, 65u})
    {
        SCOPED_TRACE(guides);
        const auto result = ParseTfx(Guides(guides, 4));
        ASSERT_TRUE(result.IsSuccess());
        EXPECT_EQ(result.Value().guideCount, guides);
        EXPECT_EQ(result.Value().positions.size(), guides * 4u);
    }
}

TEST(TfxLoader, PreservesExportedCoordinatesAndInverseMass)
{
    auto bytes = Guides();
    Point(bytes, 2, {-10, 20, -30, 0.25f});
    const auto result = ParseTfx(bytes);
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_EQ(result.Value().positions[2], (std::array<float, 4>{-10, 20, -30, 0.25f}));
    EXPECT_EQ(result.Value().boundsMin, (std::array<float, 3>{-10, 0, -30}));
    EXPECT_EQ(result.Value().boundsMax, (std::array<float, 3>{0, 20, -2}));
}

TEST(TfxLoader, ZeroOptionalOffsetsProduceEmptyArrays)
{
    const auto result = ParseTfx(Guides());
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_TRUE(result.Value().strandUVs.empty());
    EXPECT_TRUE(result.Value().vertexUVs.empty());
    EXPECT_TRUE(result.Value().strandThickness.empty());
    EXPECT_TRUE(result.Value().vertexColors.empty());
}

TEST(TfxLoader, ReadsEveryOptionalAttributeWithoutChangingUvOrColor)
{
    auto bytes = Guides();
    Attribute(bytes, 16, {-0.25f, 1.25f});
    Attribute(bytes, 20, {0, 0, 0.5f, 0.25f, 1, 0.75f, 2, 1});
    Attribute(bytes, 24, {0.2f});
    Attribute(bytes, 28, {1, 0, 0, 1, 0, 1, 0, 0.5f, 0, 0, 1, 1, 2, 3, 4, 0});
    const auto result = ParseTfx(bytes);
    ASSERT_TRUE(result.IsSuccess());
    const auto& asset = result.Value();
    EXPECT_EQ(asset.strandUVs[0], (std::array<float, 2>{-0.25f, 1.25f}));
    EXPECT_EQ(asset.vertexUVs[3], (std::array<float, 2>{2, 1}));
    EXPECT_FLOAT_EQ(asset.strandThickness[0], 0.2f);
    EXPECT_EQ(asset.vertexColors[3], (std::array<float, 4>{2, 3, 4, 0}));
}

TEST(TfxLoader, UsesAbsoluteOffsetsInsteadOfAssumingBlockOrder)
{
    auto bytes = Guides();
    bytes.insert(bytes.begin() + 160, 16, 0);
    U32(bytes, 12, 176);
    U32(bytes, 16, 160);
    Float(bytes, 160, 0.125f);
    Float(bytes, 164, 0.75f);
    const auto result = ParseTfx(bytes);
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_FLOAT_EQ(result.Value().positions[3][1], 3);
    EXPECT_EQ(result.Value().strandUVs[0], (std::array<float, 2>{0.125f, 0.75f}));
}

TEST(TfxLoader, AcceptsKnownLayoutWithNewerVersionAndReservedData)
{
    auto bytes = Guides();
    Float(bytes, 0, 5.0f);
    U32(bytes, 32, 123);
    bytes.resize(bytes.size() + 8, 42);
    const auto result = ParseTfx(bytes);
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_FLOAT_EQ(result.Value().version, 5);
}

TEST(TfxLoader, AcceptsEverySupportedStrandResolution)
{
    for (const auto points : {4u, 8u, 16u, 32u, 64u})
    {
        SCOPED_TRACE(points);
        const auto result = ParseTfx(Guides(1, points));
        ASSERT_TRUE(result.IsSuccess());
        EXPECT_EQ(result.Value().positions.size(), points);
    }
}

TEST(TfxLoader, RejectsEveryTruncatedHeader)
{
    auto bytes = Guides();
    for (std::size_t size = 0; size < 160; ++size)
    {
        SCOPED_TRACE(size);
        ExpectError({bytes.begin(), bytes.begin() + size}, "Hair.TfxTruncated", "header");
    }
}

TEST(TfxLoader, RejectsLegacyOrNonFiniteVersion)
{
    for (const auto version : {3.9f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()})
    {
        auto bytes = Guides();
        Float(bytes, 0, version);
        ExpectError(bytes, "Hair.TfxVersion", "version");
    }
}

TEST(TfxLoader, RejectsZeroGuides)
{
    auto bytes = Guides();
    U32(bytes, 4, 0);
    ExpectError(bytes, "Hair.TfxCount", "guideCount");
}

TEST(TfxLoader, RejectsUnsupportedStrandResolution)
{
    for (const auto points : {0u, 2u, 3u, 5u, 65u, 0xffffffffu})
    {
        auto bytes = Guides();
        U32(bytes, 8, points);
        ExpectError(bytes, "Hair.TfxCount", "pointsPerStrand");
    }
}

TEST(TfxLoader, RejectsOversizedGuideCountBeforeAllocating)
{
    auto bytes = Guides();
    U32(bytes, 4, 0xffffffffu);
    ExpectError(bytes, "Hair.TfxBudget", "guideCount");
}

TEST(TfxLoader, RejectsMissingPositionBlock)
{
    auto bytes = Guides();
    U32(bytes, 12, 0);
    ExpectError(bytes, "Hair.TfxOffset", "positions");
}

TEST(TfxLoader, RejectsBlockInsideHeader)
{
    auto bytes = Guides();
    U32(bytes, 12, 152);
    ExpectError(bytes, "Hair.TfxOffset", "positions");
}

TEST(TfxLoader, RejectsMisalignedBlock)
{
    auto bytes = Guides();
    U32(bytes, 12, 164);
    ExpectError(bytes, "Hair.TfxOffset", "positions");
}

TEST(TfxLoader, RejectsTruncatedPositionPayload)
{
    auto bytes = Guides();
    bytes.pop_back();
    ExpectError(bytes, "Hair.TfxTruncated", "positions");
}

TEST(TfxLoader, RejectsOutOfBoundsOptionalOffsetWithoutIntegerWrap)
{
    auto bytes = Guides();
    U32(bytes, 28, 0xfffffff8u);
    ExpectError(bytes, "Hair.TfxTruncated", "vertexColors");
}

TEST(TfxLoader, RejectsTruncatedOptionalPayload)
{
    auto bytes = Guides();
    Attribute(bytes, 20, {0, 1});
    ExpectError(bytes, "Hair.TfxTruncated", "vertexUVs");
}

TEST(TfxLoader, RejectsOverlappingAttributes)
{
    auto bytes = Guides();
    U32(bytes, 16, 160);
    ExpectError(bytes, "Hair.TfxOverlap", "strandUVs");
}

TEST(TfxLoader, RejectsNonFinitePositionOrInverseMass)
{
    for (const auto component : {0u, 1u, 2u, 3u})
    {
        auto bytes = Guides();
        Float(bytes, 160 + component * 4, std::numeric_limits<float>::quiet_NaN());
        ExpectError(bytes, "Hair.TfxNonFinite", "positions");
    }
}

TEST(TfxLoader, RejectsNegativeInverseMass)
{
    auto bytes = Guides();
    Float(bytes, 172, -1);
    ExpectError(bytes, "Hair.TfxInverseMass", "positions");
}

TEST(TfxLoader, RejectsNonFiniteUv)
{
    auto bytes = Guides();
    Attribute(bytes, 16, {0, std::numeric_limits<float>::infinity()});
    ExpectError(bytes, "Hair.TfxNonFinite", "strandUVs");
}

TEST(TfxLoader, RejectsNonFiniteVertexColor)
{
    auto bytes = Guides();
    std::vector<float> colors(16, 1);
    colors[15] = std::numeric_limits<float>::quiet_NaN();
    Attribute(bytes, 28, colors);
    ExpectError(bytes, "Hair.TfxNonFinite", "vertexColors");
}

TEST(TfxLoader, RejectsInvalidThickness)
{
    for (const auto thickness : {-1.0f, std::numeric_limits<float>::infinity()})
    {
        auto bytes = Guides();
        Attribute(bytes, 24, {thickness});
        ExpectError(bytes, "Hair.TfxThickness", "strandThickness");
    }
}

// 每个测试拥有独立目录，析构时只清理本夹具创建的输入。
class TfxFileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("vl-tfx-" + std::to_string(stamp));
        ownsRoot = std::filesystem::create_directory(root);
        ASSERT_TRUE(ownsRoot);
    }
    void TearDown() override
    {
        if (ownsRoot)
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    }
    std::filesystem::path root;
    bool ownsRoot = false;
};

TEST_F(TfxFileTest, LoadsBinaryFileThroughFilesystemPath)
{
    const auto path = root / std::filesystem::u8path(u8"发丝.tfx");
    const auto bytes = Guides(2, 16);
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ASSERT_TRUE(output);
    }
    const auto result = LoadTfx(path);
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_EQ(result.Value().guideCount, 2u);
    EXPECT_EQ(result.Value().positions.size(), 32u);
}

TEST_F(TfxFileTest, MissingFileReturnsStructuredPathError)
{
    const auto path = root / "missing.tfx";
    const auto result = LoadTfx(path);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Hair.TfxRead");
    EXPECT_EQ(result.Error().sourcePath, path.u8string());
}

TEST_F(TfxFileTest, EmptyFileReturnsHeaderError)
{
    const auto path = root / "empty.tfx";
    std::ofstream{path, std::ios::binary}.close();
    const auto result = LoadTfx(path);
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Hair.TfxTruncated");
}
}
