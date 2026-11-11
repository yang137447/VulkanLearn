#include "terrain/terrainAssetLoader.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "terrain/terrainPaintFile.h"
#include "terrain/terrainHeightmapIO.h"
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
        desc.paintPath = asset.value("paintPath", std::string());
        desc.size = Eigen::Vector2f(
            asset.at("size")[0].get<float>(),
            asset.at("size")[1].get<float>());
        desc.heightScale = asset.at("heightScale").get<float>();
        desc.heightOffset = asset.at("heightOffset").get<float>();
        desc.gridResolution = asset.at("gridResolution").get<uint32_t>();
        desc.chunkQuads = asset.at("chunkQuads").get<uint32_t>();
        desc.heightmapMargin = asset.value("heightmapMargin", 0u);
        // CPU 几何 LOD：validator 已保证级数与步长/距离表合法。旧 TR 不写时
        // lodLevels=1、距离表为空，网格生成与历史行为完全一致。
        desc.lodLevels = asset.value("lodLevels", 1u);
        if (asset.contains("lodDistances"))
        {
            for (const auto& distanceJson : asset.at("lodDistances"))
                desc.lodDistancesCm.push_back(distanceJson.get<float>());
        }
        if (asset.contains("chunkResidencyDistances"))
        {
            for (const auto& distanceJson :
                 asset.at("chunkResidencyDistances"))
            {
                desc.chunkResidencyDistancesCm.push_back(
                    distanceJson.get<float>());
            }
        }
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
                // 可选跨 TR 共享身份；缺省在 registry 里回退成
                // "<assetPath>#<name>"，旧 TR 不写该字段时行为不变。
                layer.key = layerJson.value("key", std::string());
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

        const std::filesystem::path heightmapPath =
            CommonFunction::Path(desc.heightmapPath);
        std::string heightmapError;
        auto heightmap = TerrainHeightmapIO::Load(heightmapPath, heightmapError);
        if (!heightmap)
        {
            throw std::runtime_error(
                heightmapError.empty() ?
                    "Failed to decode heightmap: " + desc.heightmapPath :
                    heightmapError);
        }
        if (heightmap->width < 2 || heightmap->height < 2)
            throw std::runtime_error("Heightmap requires at least 2x2 pixels: " + desc.heightmapPath);
        if (desc.heightmapMargin > 0)
        {
            const uint32_t expected =
                desc.gridResolution + 2 * desc.heightmapMargin + 1;
            if (heightmap->width != expected || heightmap->height != expected)
            {
                throw std::runtime_error(
                    "Heightmap with heightmapMargin requires exactly " +
                    std::to_string(expected) + "x" +
                    std::to_string(expected) + " pixels: " +
                    desc.heightmapPath);
            }
        }

        TerrainBuildPlan plan;
        plan.desc = std::move(desc);
        plan.heightmap = std::make_shared<const HostImage>(std::move(*heightmap));
        // paintPath 可以指向尚不存在的文件：它是 Save Paint 的写入目标。
        // 只有文件已经存在时才恢复快照；存在但内容非法必须失败，不能静默
        // 当作“未绘制”，否则一次损坏的保存会被下一次编辑覆盖。
        if (!plan.desc.paintPath.empty())
        {
            if (plan.desc.layers.empty())
                throw std::runtime_error(
                    "Terrain paintPath requires at least one layer: " +
                    plan.desc.paintPath);
            // paintPath 与 Save Paint 共用同一套资源根解析/越界检查，不能再走
            // CommonFunction::Path 的 shader/项目根回退，否则读取与写回可能
            // 命中不同文件，保存后重载却看不到新快照。
            std::filesystem::path paintPhysical;
            std::string paintPathError;
            if (!ResolveTerrainPaintPath(
                    CommonFunction::GetResourcePath(),
                    plan.desc.paintPath,
                    paintPhysical,
                    paintPathError))
                throw std::runtime_error(paintPathError);
            std::error_code paintQueryError;
            const bool paintExists =
                std::filesystem::exists(paintPhysical, paintQueryError);
            if (paintQueryError)
                throw std::runtime_error(
                    "Terrain paintPath cannot be queried: " + paintPathError);
            if (paintExists &&
                !std::filesystem::is_regular_file(paintPhysical, paintQueryError))
                throw std::runtime_error(
                    "Terrain paintPath must be a regular file: " +
                    plan.desc.paintPath);
            if (paintExists)
            {
                auto paint = std::make_shared<TerrainLayerPaintData>();
                std::string paintError;
                if (!LoadTerrainLayerPaintFile(
                        paintPhysical,
                        static_cast<uint32_t>(plan.desc.layers.size()),
                        *paint,
                        paintError))
                    throw std::runtime_error(paintError);
                plan.layerPaint = std::move(paint);
            }
        }
        return plan;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(
            "Terrain asset load failed: " + pathString + ": " + error.what());
    }
}

} // namespace VL
