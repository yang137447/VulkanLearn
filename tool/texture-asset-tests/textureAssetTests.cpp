#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "textureAssetLoader.h"
#include "resource/image/textureIO.h"

namespace
{
    void Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void RequireThrows(
        const std::function<void()>& callback,
        const std::string& message)
    {
        try
        {
            callback();
        }
        catch (const std::exception&)
        {
            return;
        }
        throw std::runtime_error(message);
    }

    nlohmann::json BuildTextureAssetJson()
    {
        return {
            {"name", "T_textureAssetGate"},
            {"type", "texture"},
            {"source", "Generated/Validation/texture-asset-tests/T_textureAssetGate.png"},
            {"colorSpace", "srgb"},
            {"mipmaps", true},
            {"filter", "linear"},
            {"wrapMode", "repeat"},
            {"channelsDescription", {
                {"r", "baseColor.r"},
                {"g", "baseColor.g"},
                {"b", "baseColor.b"},
                {"a", "opacity"}}}};
    }

    void WriteTextureAsset(
        const std::filesystem::path& path,
        const nlohmann::json& textureAssetJson)
    {
        std::ofstream stream(path);
        if (!stream.is_open())
        {
            throw std::runtime_error("Failed to create texture asset test fixture");
        }
        stream << textureAssetJson.dump(2);
    }

    void TestTextureAssetFieldGate(const std::filesystem::path& fixturePath)
    {
        nlohmann::json textureAssetJson = BuildTextureAssetJson();
        WriteTextureAsset(fixturePath, textureAssetJson);
        const TextureBindingLoadDesc loadDesc =
            LoadTextureAssetDesc(fixturePath.string());
        Require(
            BuildTextureCacheKey(loadDesc).find("flipY") == std::string::npos,
            "texture cache identity still contains flipY");

        textureAssetJson["flipY"] = false;
        WriteTextureAsset(fixturePath, textureAssetJson);
        RequireThrows(
            [&fixturePath]() {
                LoadTextureAssetDesc(fixturePath.string());
            },
            "texture asset gate accepted flipY");

        textureAssetJson = BuildTextureAssetJson();
        textureAssetJson["unknownField"] = 1;
        WriteTextureAsset(fixturePath, textureAssetJson);
        RequireThrows(
            [&fixturePath]() {
                LoadTextureAssetDesc(fixturePath.string());
            },
            "texture asset gate accepted an unknown field");
    }

    void TestTextureIoVerticalFlip()
    {
        static constexpr std::array<uint8_t, 62> bmpBytes = {
            0x42, 0x4d, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22,
            0x33, 0x00, 0xaa, 0xbb, 0xcc, 0x00};

        TextureIO::LoadOptions unchangedOptions;
        unchangedOptions.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
        unchangedOptions.forceChannels = 4;
        TextureIO::LoadOptions flippedOptions = unchangedOptions;
        flippedOptions.flipY = TextureIO::LoadOptions::FlipYMode::ForceOn;

        const std::optional<HostImage> unchanged = TextureIO::LoadFromMemory(
            bmpBytes.data(),
            bmpBytes.size(),
            unchangedOptions);
        const std::optional<HostImage> flipped = TextureIO::LoadFromMemory(
            bmpBytes.data(),
            bmpBytes.size(),
            flippedOptions);
        Require(unchanged.has_value() && flipped.has_value(), "failed to decode flip test BMP");
        Require(
            unchanged->width == 1 && unchanged->height == 2 &&
            unchanged->rowStrideBytes == 4,
            "flip test BMP decoded with unexpected dimensions");
        for (size_t channel = 0; channel < unchanged->rowStrideBytes; ++channel)
        {
            Require(
                unchanged->data[channel] == flipped->data[unchanged->rowStrideBytes + channel] &&
                unchanged->data[unchanged->rowStrideBytes + channel] == flipped->data[channel],
                "TextureIO ForceOn did not swap decoded image rows");
        }
    }
} // namespace

class TextureAssetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        fixtureDirectory =
            std::filesystem::temp_directory_path() /
            "vulkanlearn_texture_asset_tests";
        fixturePath = fixtureDirectory / "T_textureAssetGate.json";
        std::filesystem::remove_all(fixtureDirectory);
        std::filesystem::create_directories(fixtureDirectory);
    }

    void TearDown() override
    {
        std::filesystem::remove_all(fixtureDirectory);
    }

    std::filesystem::path fixtureDirectory;
    std::filesystem::path fixturePath;
};

TEST_F(TextureAssetTest, FieldGate)
{
    TestTextureAssetFieldGate(fixturePath);
}

TEST_F(TextureAssetTest, VerticalFlip)
{
    TestTextureIoVerticalFlip();
}
