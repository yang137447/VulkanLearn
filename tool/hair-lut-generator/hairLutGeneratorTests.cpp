#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "render/hair/hairAssets.h"
#include "render/hair/hairConventions.h"
#include "render/hair/hairLutCoordinates.h"

namespace
{

VL::HairAzimuthalLutMetadata LoadFixtureMetadata()
{
    const std::filesystem::path fixturePath =
        std::filesystem::current_path() /
        "fixtures_hair_author_metadata.json";
    std::ifstream file(fixturePath);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open Hair LUT metadata fixture: " + fixturePath.string());
    }

    nlohmann::json json;
    file >> json;
    return VL::ParseHairAzimuthalLutMetadata(json, fixturePath.string());
}

} // namespace

TEST(HairLutGenerator, ValidatesAuthorMetadata)
{
    const VL::HairAzimuthalLutMetadata metadata = LoadFixtureMetadata();
    EXPECT_EQ(metadata.width, VL::HairAzimuthalLutWidth);
    EXPECT_EQ(metadata.height, VL::HairAzimuthalLutHeight);
    EXPECT_EQ(metadata.layers, VL::HairAzimuthalLutLayerCount);
}

TEST(HairLutGenerator, BuildsFrozenDispatchContract)
{
    const VL::HairAzimuthalLutMetadata metadata = LoadFixtureMetadata();
    const uint32_t groupsX = (metadata.width + 7u) / 8u;
    const uint32_t groupsY = (metadata.height + 7u) / 8u;
    const VL::Hair::HairLutUv center =
        VL::Hair::EncodeHairAzimuthalLutUv(0.0f, 0.0f, 0.0f);
    const VL::Hair::HairLutUv upper =
        VL::Hair::EncodeHairAzimuthalLutUv(
            VL::Hair::HairPi,
            VL::Hair::HairHalfPi,
            1.0f);

    EXPECT_EQ(groupsX, (VL::HairAzimuthalLutWidth + 7u) / 8u);
    EXPECT_EQ(groupsY, (VL::HairAzimuthalLutHeight + 7u) / 8u);
    EXPECT_EQ(metadata.layers, VL::HairAzimuthalLutLayerCount);
    EXPECT_TRUE(std::isfinite(center.u));
    EXPECT_TRUE(std::isfinite(center.v));
    EXPECT_TRUE(std::isfinite(upper.u));
    EXPECT_TRUE(std::isfinite(upper.v));
}
