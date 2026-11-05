#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "mesh/vegetationAsset.h"

namespace
{

class VegetationAssetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        directory =
            std::filesystem::temp_directory_path() /
            ("vl_vegetation_asset_" +
             std::to_string(
                 std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
    }

    void TearDown() override
    {
        std::filesystem::remove_all(directory);
    }

    std::filesystem::path Write(
        const std::string& name,
        const nlohmann::json& value)
    {
        const std::filesystem::path path = directory / name;
        std::ofstream file(path);
        file << value;
        return path;
    }

    nlohmann::json MakeAsset() const
    {
        nlohmann::json asset = {
            {"version", 1},
            {"mesh", "Generated/Vegetation/SM_test.json"},
            {"instances", nlohmann::json::array()}
        };
        nlohmann::json first = {
            {"position", {1.0f, 2.0f, 3.0f}},
            {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}},
            {"scale", 2.0f},
            {"windPhase", 0.25f},
            {"lodBias", -0.5f},
            {"variant", 3},
            {"seed", 7}
        };
        nlohmann::json second = {
            {"position", {-4.0f, 5.0f, 6.0f}},
            {"rotation", {0.0f, 0.70710678f, 0.0f, 0.70710678f}},
            {"scale", {1.0f, 2.0f, 3.0f}}
        };
        asset["instances"].push_back(first);
        asset["instances"].push_back(second);
        return asset;
    }

    std::filesystem::path directory;
};

TEST_F(VegetationAssetTest, LoadsAndBuildsInstanceStream)
{
    const std::filesystem::path path =
        Write("VEG_test.json", MakeAsset());

    VL::VegetationAsset asset;
    std::string error;
    ASSERT_TRUE(VL::LoadVegetationAsset(path, asset, error)) << error;
    ASSERT_EQ(asset.meshPath, "Generated/Vegetation/SM_test.json");
    ASSERT_EQ(asset.instances.size(), 2u);

    const std::vector<InstanceVertex> vertices =
        VL::BuildVegetationInstanceVertices(asset);
    ASSERT_EQ(vertices.size(), 2u);
    EXPECT_NEAR(vertices[0].model(0, 0), 2.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].model(1, 1), 2.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].model(2, 2), 2.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].model(0, 3), 1.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].model(1, 3), 2.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].model(2, 3), 3.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].params.x(), 0.25f, 1.0e-6f);
    EXPECT_NEAR(vertices[0].params.y(), -0.5f, 1.0e-6f);
    EXPECT_NEAR(vertices[0].params.z(), 3.0f, 1.0e-6f);
    EXPECT_NEAR(vertices[0].params.w(), 7.0f, 1.0e-6f);

    // Y 轴 90°：local X 转到 world -Z，local Z 转到 world X。
    EXPECT_NEAR(vertices[1].model(0, 0), 0.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[1].model(0, 2), 3.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[1].model(2, 0), -1.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[1].model(2, 2), 0.0f, 1.0e-5f);
}

TEST_F(VegetationAssetTest, NormalizesQuaternion)
{
    nlohmann::json assetJson = MakeAsset();
    assetJson["instances"] = {
        {
            {"position", {0.0f, 0.0f, 0.0f}},
            {"rotation", {0.0f, 0.0f, 0.0f, 2.0f}}
        }
    };
    const std::filesystem::path path =
        Write("VEG_normalized.json", assetJson);

    VL::VegetationAsset asset;
    std::string error;
    ASSERT_TRUE(VL::LoadVegetationAsset(path, asset, error)) << error;
    const std::vector<InstanceVertex> vertices =
        VL::BuildVegetationInstanceVertices(asset);
    ASSERT_EQ(vertices.size(), 1u);
    EXPECT_NEAR(vertices[0].model(0, 0), 1.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].model(1, 1), 1.0f, 1.0e-5f);
    EXPECT_NEAR(vertices[0].model(2, 2), 1.0f, 1.0e-5f);
}

TEST_F(VegetationAssetTest, AccumulatesTransformedBounds)
{
    nlohmann::json assetJson = MakeAsset();
    assetJson["instances"] = {
        {
            {"position", {10.0f, 0.0f, 0.0f}},
            {"scale", 2.0f}
        },
        {
            {"position", {-10.0f, 0.0f, 0.0f}},
            {"scale", 1.0f}
        }
    };
    const std::filesystem::path path =
        Write("VEG_bounds.json", assetJson);

    VL::VegetationAsset asset;
    std::string error;
    ASSERT_TRUE(VL::LoadVegetationAsset(path, asset, error)) << error;
    Eigen::Vector3f boundsMin;
    Eigen::Vector3f boundsMax;
    VL::AccumulateVegetationBounds(
        asset,
        Eigen::Vector3f(-1.0f, -1.0f, -1.0f),
        Eigen::Vector3f(1.0f, 1.0f, 1.0f),
        boundsMin,
        boundsMax);
    EXPECT_TRUE(boundsMin.isApprox(Eigen::Vector3f(-11.0f, -2.0f, -2.0f)));
    EXPECT_TRUE(boundsMax.isApprox(Eigen::Vector3f(12.0f, 2.0f, 2.0f)));
}

TEST_F(VegetationAssetTest, RejectsMissingMesh)
{
    nlohmann::json assetJson = MakeAsset();
    assetJson.erase("mesh");
    const std::filesystem::path path =
        Write("VEG_missing_mesh.json", assetJson);

    VL::VegetationAsset asset;
    std::string error;
    EXPECT_FALSE(VL::LoadVegetationAsset(path, asset, error));
    EXPECT_NE(error.find("mesh"), std::string::npos);
}

TEST_F(VegetationAssetTest, RejectsMissingPosition)
{
    nlohmann::json assetJson = MakeAsset();
    assetJson["instances"][0].erase("position");
    const std::filesystem::path path =
        Write("VEG_missing_position.json", assetJson);

    VL::VegetationAsset asset;
    std::string error;
    EXPECT_FALSE(VL::LoadVegetationAsset(path, asset, error));
    EXPECT_NE(error.find("position"), std::string::npos);
}

TEST_F(VegetationAssetTest, RejectsZeroScale)
{
    nlohmann::json assetJson = MakeAsset();
    assetJson["instances"][0]["scale"] = {1.0f, 0.0f, 1.0f};
    const std::filesystem::path path =
        Write("VEG_zero_scale.json", assetJson);

    VL::VegetationAsset asset;
    std::string error;
    EXPECT_FALSE(VL::LoadVegetationAsset(path, asset, error));
    EXPECT_NE(error.find("scale"), std::string::npos);
}

TEST_F(VegetationAssetTest, RejectsInvalidParameters)
{
    nlohmann::json assetJson = MakeAsset();
    assetJson["instances"][0]["variant"] = -1;
    const std::filesystem::path path =
        Write("VEG_invalid_variant.json", assetJson);

    VL::VegetationAsset asset;
    std::string error;
    EXPECT_FALSE(VL::LoadVegetationAsset(path, asset, error));
    EXPECT_NE(error.find("variant"), std::string::npos);
}

TEST_F(VegetationAssetTest, RejectsEmptyInstanceList)
{
    nlohmann::json assetJson = MakeAsset();
    assetJson["instances"] = nlohmann::json::array();
    const std::filesystem::path path =
        Write("VEG_empty.json", assetJson);

    VL::VegetationAsset asset;
    std::string error;
    EXPECT_FALSE(VL::LoadVegetationAsset(path, asset, error));
    EXPECT_NE(error.find("1..100000"), std::string::npos);
}

} // namespace
