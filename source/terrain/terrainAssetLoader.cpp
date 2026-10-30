#include "terrain/terrainAssetLoader.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "resource/image/textureIO.h"
#include "terrain/validation/terrainAssetValidator.h"

namespace VL
{

TerrainBuildPlan TerrainAssetLoader::Prepare(std::string_view terrainAssetPath)
{
    const std::string pathString(terrainAssetPath);
    try
    {
        const std::string physicalPath = CommonFunction::Path(pathString);
        std::ifstream file(physicalPath, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open terrain asset");

        nlohmann::json asset;
        file >> asset;
        TerrainAssetValidator::Validate(asset, pathString);

        TerrainAssetDesc desc;
        desc.assetPath = pathString;
        desc.name = asset.at("name").get<std::string>();
        desc.heightmapPath = asset.at("heightmapPath").get<std::string>();
        desc.size = Eigen::Vector2f(
            asset.at("size")[0].get<float>(),
            asset.at("size")[1].get<float>());
        desc.heightScale = asset.at("heightScale").get<float>();
        desc.heightOffset = asset.at("heightOffset").get<float>();
        desc.gridResolution = asset.at("gridResolution").get<uint32_t>();
        desc.chunkQuads = asset.at("chunkQuads").get<uint32_t>();
        desc.pageTilesPerSide = asset.value("pageTilesPerSide", 8u);
        if (asset.contains("pageOrigin"))
        {
            desc.pageOrigin = Eigen::Vector2i(
                asset.at("pageOrigin")[0].get<int>(),
                asset.at("pageOrigin")[1].get<int>());
        }
        desc.materialInstancePath = asset.at("materialInstancePath").get<std::string>();
        if (asset.contains("layers"))
        {
            for (const auto& layerJson : asset.at("layers"))
            {
                TerrainLayerDesc layer;
                layer.name = layerJson.at("name").get<std::string>();
                layer.heightMin = layerJson.at("heightRange")[0].get<float>();
                layer.heightMax = layerJson.at("heightRange")[1].get<float>();
                layer.slopeMin = layerJson.at("slopeRange")[0].get<float>();
                layer.slopeMax = layerJson.at("slopeRange")[1].get<float>();
                layer.noise = layerJson.value("noise", 0.0f);
                layer.uvScale = layerJson.value("uvScale", 1.0f);
                layer.uvRotation = layerJson.value("uvRotation", 0.0f);
                // L1 可选字段；validator 已保证取值范围，这里只做取值。
                layer.sourceResolution = layerJson.value("sourceResolution", 1024u);
                layer.sourceSet = layerJson.value("sourceSet", 0u);
                layer.flags = layerJson.value("flags", 0u);
                if (layerJson.contains("avgColor"))
                {
                    layer.avgColor = Eigen::Vector3f(
                        layerJson.at("avgColor")[0].get<float>(),
                        layerJson.at("avgColor")[1].get<float>(),
                        layerJson.at("avgColor")[2].get<float>());
                }
                desc.layers.push_back(std::move(layer));
            }
        }

        // 材质路径在场景预检阶段就要可解析；完整 MI/shader 校验仍留给正式 material loader。
        static_cast<void>(CommonFunction::Path(desc.materialInstancePath));

        TextureIO::LoadOptions options;
        options.semantic = HostImage::TextureSemantic::Data;
        options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
        options.transfer = TextureIO::LoadOptions::Transfer::Linear;
        options.forceChannels = 4;
        const std::filesystem::path heightmapPath =
            CommonFunction::Path(desc.heightmapPath);
        auto heightmap = TextureIO::Load(heightmapPath, options);
        if (!heightmap)
            throw std::runtime_error("Failed to decode heightmap: " + desc.heightmapPath);
        if (heightmap->width < 2 || heightmap->height < 2)
            throw std::runtime_error("Heightmap requires at least 2x2 pixels: " + desc.heightmapPath);

        TerrainBuildPlan plan;
        plan.desc = std::move(desc);
        plan.heightmap = std::make_shared<const HostImage>(std::move(*heightmap));
        return plan;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(
            "Terrain asset load failed: " + pathString + ": " + error.what());
    }
}

} // namespace VL
