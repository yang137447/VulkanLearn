#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "resource/image/textureIO.h"
#include "terrain/terrainAssetLoader.h"
#include "terrain/validation/terrainAssetValidator.h"
#include "terrain/terrainCompositedVT.h"

namespace
{

nlohmann::json CompositedTerrainJson()
{
    return {
        {"name", "TR_composited_asset_test"}, {"type", "terrain"},
        {"compositedVT", {{"enabled", true}, {"packagePath", "Generated/Validation/CompositedAsset/Cooked/manifest.json"}}},
        {"heightmapPath", "Generated/Validation/height.png"},
        {"materialInstancePath", "Generated/Validation/MI_terrain.json"},
        {"size", {1000.0, 1000.0}}, {"heightScale", 100.0},
        {"heightOffset", 0.0}, {"gridResolution", 2}, {"chunkQuads", 1}};
}

void ValidateComposited(const nlohmann::json& configuration)
{
    auto asset = CompositedTerrainJson();
    if (configuration.is_object())
        asset["compositedVT"].update(configuration);
    else
        asset["compositedVT"] = configuration;
    VL::TerrainAssetValidator::Validate(asset, "TR_composited_asset_test.json");
}

class TerrainCompositedAssetLoaderTest : public testing::Test
{
protected:
    void SetUp() override
    {
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
            ("vl-composited-asset-" + std::to_string(id));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        HostImage image;
        image.width = 2;
        image.height = 2;
        image.rowStrideBytes = 8;
        image.data.assign(16, 128);
        TextureIO::SaveOptions options;
        options.format = TextureIO::FileFormat::Png;
        ASSERT_TRUE(TextureIO::Save(directory / "height.png", image, options));
        std::ofstream material(directory / "MI_terrain.json");
        material << R"({"type":"materialInstance","material":"shader/glsl/M_unlit.json"})";
        ASSERT_TRUE(material.good());
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    VL::TerrainAssetDesc Prepare(nlohmann::json asset)
    {
        asset["heightmapPath"] = (directory / "height.png").string();
        asset["materialInstancePath"] = (directory / "MI_terrain.json").string();
        const auto path = directory / "TR_terrain.json";
        std::ofstream stream(path);
        stream << asset.dump();
        stream.close();
        return VL::TerrainAssetLoader::Prepare(path.string()).desc;
    }

    std::filesystem::path directory;
};

} // namespace

TEST_F(TerrainCompositedAssetLoaderTest, RejectsLegacyDisabledAndIncompleteConfiguration)
{
    auto asset = CompositedTerrainJson();
    for (const auto configuration : {nlohmann::json::object(),
        nlohmann::json{{"enabled", false}, {"packagePath", "Generated/manifest.json"}},
        nlohmann::json{{"packagePath", "Generated/manifest.json"}}})
    {
        asset["compositedVT"] = configuration;
        EXPECT_THROW(Prepare(asset), std::exception);
    }
    asset.erase("compositedVT");
    EXPECT_THROW(Prepare(asset), std::exception);
}

TEST(TerrainCompositedProductionOutputTest, RejectsCompressedOrMismatchedRuntimeOutput)
{
    VL::TerrainCompositedVTBuildDesc surface;
    surface.channels = {VL::TerrainCompositedVTFormat::RGBA8Srgb,
        VL::TerrainCompositedVTFormat::RGBA8UNorm, VL::TerrainCompositedVTFormat::RGBA8UNorm};
    EXPECT_NO_THROW(VL::ValidateTerrainCompositedProductionOutput(surface));
    for (const auto channels : {
        std::vector<VL::TerrainCompositedVTFormat>{VL::TerrainCompositedVTFormat::BC7Srgb,
            VL::TerrainCompositedVTFormat::BC5UNorm, VL::TerrainCompositedVTFormat::RGBA8UNorm},
        std::vector<VL::TerrainCompositedVTFormat>{VL::TerrainCompositedVTFormat::RGBA8UNorm,
            VL::TerrainCompositedVTFormat::RGBA8UNorm, VL::TerrainCompositedVTFormat::RGBA8UNorm},
        std::vector<VL::TerrainCompositedVTFormat>{VL::TerrainCompositedVTFormat::RGBA8Srgb,
            VL::TerrainCompositedVTFormat::RG8UNorm, VL::TerrainCompositedVTFormat::RGBA8UNorm}})
    {
        auto unsupported = surface;
        unsupported.channels = channels;
        EXPECT_THROW(VL::ValidateTerrainCompositedProductionOutput(unsupported), std::invalid_argument);
    }
    surface.generatorVersion = 2;
    EXPECT_THROW(VL::ValidateTerrainCompositedProductionOutput(surface), std::invalid_argument);
    surface.generatorVersion = VL::TerrainCompositedGeneratorVersion;
    surface.encoding = "gpu-center-mip0-area4-coarse-rgba8-height-fade1-v2";
    EXPECT_THROW(VL::ValidateTerrainCompositedProductionOutput(surface), std::invalid_argument);
}

TEST_F(TerrainCompositedAssetLoaderTest, ParsesAuthoredFieldsWithoutOpeningPackage)
{
    auto asset = CompositedTerrainJson();
    asset["compositedVT"] = {
        {"enabled", true}, {"packagePath", "Generated/NotCooked/manifest.json"},
        {"virtualResolution", {8192, 128}},
        {"productionSourcePath", "Terrain/production.json"}};
    const auto configuration = Prepare(asset).compositedVT;
    EXPECT_TRUE(configuration.enabled);
    EXPECT_EQ(configuration.packagePath, "Generated/NotCooked/manifest.json");
    EXPECT_EQ(configuration.virtualResolution.x(), 8192);
    EXPECT_EQ(configuration.virtualResolution.y(), 128);
    EXPECT_EQ(configuration.productionSourcePath, "Terrain/production.json");
}

TEST_F(TerrainCompositedAssetLoaderTest, EnabledWithoutPackageIsRejected)
{
    auto asset = CompositedTerrainJson();
    asset["compositedVT"] = {{"enabled", true}};
    EXPECT_THROW(Prepare(asset), std::exception);
}

TEST(TerrainCompositedAssetValidation, RequiresObjectAndKnownFields)
{
    for (const auto& value : {nlohmann::json(nullptr), nlohmann::json(false),
         nlohmann::json(1), nlohmann::json("enabled"), nlohmann::json::array()})
        EXPECT_THROW(ValidateComposited(value), std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"enable", true}}), std::invalid_argument);
}

TEST(TerrainCompositedAssetValidation, EnabledMustBeBoolean)
{
    for (const auto& value : {nlohmann::json(nullptr), nlohmann::json(0),
         nlohmann::json(1), nlohmann::json("true")})
        EXPECT_THROW(ValidateComposited({{"enabled", value}}), std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"enabled", false}}), std::invalid_argument);
    EXPECT_NO_THROW(ValidateComposited({{"enabled", true}}));
}

TEST(TerrainCompositedAssetValidation, AcceptsEverySupportedDimensionAndRectangles)
{
    for (uint32_t width = 128; width <= 8192; width *= 2)
        for (uint32_t height = 128; height <= 8192; height *= 2)
            EXPECT_NO_THROW(ValidateComposited({{"virtualResolution", {width, height}}}));
}

TEST(TerrainCompositedAssetValidation, RejectsInvalidDimensionsWithoutIntegerWrapping)
{
    for (const auto& value : {
         nlohmann::json(nullptr), nlohmann::json("1024"),
         nlohmann::json::array(), nlohmann::json({1024}),
         nlohmann::json({1024, 1024, 1024}), nlohmann::json({0, 1024}),
         nlohmann::json({64, 1024}), nlohmann::json({384, 1024}),
         nlohmann::json({8193, 1024}), nlohmann::json({1024, -128}),
         nlohmann::json({1024.0, 1024}), nlohmann::json({true, 1024}),
         nlohmann::json({std::numeric_limits<uint64_t>::max(), uint64_t{1024}})})
        EXPECT_THROW(ValidateComposited({{"virtualResolution", value}}), std::invalid_argument);
}

TEST(TerrainCompositedAssetValidation, PackagePathMustBeSafeResourceRelativeFile)
{
    for (const std::string path : {
         "", "../manifest.json", "Generated/../manifest.json",
         R"(Generated\..\manifest.json)", "/manifest.json", R"(\manifest.json)",
         "C:/manifest.json", "C:manifest.json", "//server/share/manifest.json",
         "Generated/", "."})
        EXPECT_THROW(ValidateComposited({{"packagePath", path}}), std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"packagePath", std::string("a\0b", 3)}}),
                 std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"packagePath", std::string(1025, 'a')}}),
                 std::invalid_argument);
    for (const auto& value : {nlohmann::json(nullptr), nlohmann::json(1),
         nlohmann::json(false), nlohmann::json::array()})
        EXPECT_THROW(ValidateComposited({{"packagePath", value}}), std::invalid_argument);
    EXPECT_NO_THROW(ValidateComposited({{"packagePath", "Generated/VT/manifest.json"}}));
    EXPECT_NO_THROW(ValidateComposited({{"packagePath", R"(Generated\VT\manifest.json)"}}));
}

TEST(TerrainCompositedAssetValidation, RejectsRemovedPerSurfaceResidentBudget)
{
    for (const auto& value : {nlohmann::json(0), nlohmann::json(-1),
         nlohmann::json(1.0), nlohmann::json(true), nlohmann::json(nullptr),
         nlohmann::json(uint64_t{1} << 32),
         nlohmann::json(std::numeric_limits<uint64_t>::max())})
        EXPECT_THROW(ValidateComposited({{"residentPages", value}}), std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"residentPages", 1}}), std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"residentPages", std::numeric_limits<uint32_t>::max()}}),
        std::invalid_argument);
}

TEST(TerrainCompositedAssetValidation, RejectsRemovedPerSurfaceHeightBudget)
{
    for (const auto& value : {nlohmann::json(-1), nlohmann::json(0.5),
         nlohmann::json(true), nlohmann::json(nullptr), nlohmann::json("8388608"),
         nlohmann::json(std::numeric_limits<double>::infinity())})
        EXPECT_THROW(ValidateComposited({{"heightBudgetBytes", value}}), std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"heightBudgetBytes", 0}}), std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"heightBudgetBytes", std::numeric_limits<uint64_t>::max()}}),
        std::invalid_argument);
}

TEST(TerrainCompositedAssetValidation, DisabledStillRejectsMalformedExternalData)
{
    EXPECT_THROW(ValidateComposited({{"enabled", false}, {"residentPages", 0}}),
                 std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"enabled", false}, {"virtualResolution", {256, 129}}}),
                 std::invalid_argument);
    EXPECT_THROW(ValidateComposited({{"enabled", false}, {"packagePath", "../manifest.json"}}),
                 std::invalid_argument);
}
