#include "terrain/terrainAssetLoader.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "terrain/terrainPaintFile.h"
#include "terrain/terrainPaintManifest.h"
#include "terrain/terrainHeightmapIO.h"
#include "terrain/validation/terrainAssetValidator.h"

namespace VL
{

namespace
{

// detailMaterialInstancePath 是资源根相对路径；这里与 paintPath 共用同一套
// 越界检查，避免绝对路径或 `..` 跳出资源根后被 material loader 静默解析到
// 工作目录中的另一份 MI。
bool ResolveResourceRootRelativeFile(
    const std::string& relative,
    const char* fieldName,
    std::filesystem::path& out,
    std::string& error)
{
    error.clear();
    out.clear();
    if (relative.empty())
    {
        error = std::string(fieldName) + " is empty";
        return false;
    }
    const std::filesystem::path relativePath(relative);
    if (relativePath.is_absolute() ||
        relativePath.has_root_name() ||
        relativePath.has_root_directory())
    {
        error = std::string(fieldName) +
            " must be resource-root relative: " + relative;
        return false;
    }

    std::error_code errorCode;
    const std::filesystem::path root = std::filesystem::weakly_canonical(
        CommonFunction::GetResourcePath(),
        errorCode);
    if (errorCode)
    {
        error = std::string(fieldName) +
            " resource root cannot be resolved";
        return false;
    }
    const std::filesystem::path target = std::filesystem::weakly_canonical(
        root / relativePath,
        errorCode);
    if (errorCode)
    {
        error = std::string(fieldName) +
            " cannot be resolved: " + relative;
        return false;
    }
    const std::filesystem::path relativeToRoot =
        target.lexically_relative(root);
    if (relativeToRoot.empty() || relativeToRoot.is_absolute())
    {
        error = std::string(fieldName) +
            " escapes the resource root: " + relative;
        return false;
    }
    for (const std::filesystem::path& part : relativeToRoot)
    {
        if (part == "..")
        {
            error = std::string(fieldName) +
                " escapes the resource root: " + relative;
            return false;
        }
    }
    const bool exists = std::filesystem::exists(target, errorCode);
    if (errorCode || !exists ||
        !std::filesystem::is_regular_file(target, errorCode) ||
        errorCode)
    {
        error = std::string(fieldName) +
            " does not resolve to a regular file: " + relative;
        return false;
    }
    out = target;
    return true;
}

// 只做地形加载期需要的 MI 头部识别：后续 RendererMaterialLoader 仍会执行
// 完整材质解析。这里提前失败可以避免 LOD0 chunk 生成后才发现 detail MI 是
// 普通 material / 场景等错误资产。
bool ValidateDetailMaterialInstanceFile(
    const std::filesystem::path& physicalPath,
    const std::string& relative,
    std::string& error)
{
    error.clear();
    std::ifstream file(physicalPath, std::ios::binary);
    if (!file)
    {
        error = "detailMaterialInstancePath cannot be opened: " + relative;
        return false;
    }
    nlohmann::json json;
    try
    {
        file >> json;
    }
    catch (const std::exception& exception)
    {
        error = "detailMaterialInstancePath is not valid JSON: " + relative +
            ": " + exception.what();
        return false;
    }
    if (!json.is_object() ||
        json.value("type", std::string()) != "materialInstance")
    {
        error = "detailMaterialInstancePath must resolve to type "
            "\"materialInstance\": " + relative;
        return false;
    }
    if (!json.contains("material") ||
        !json.at("material").is_string() ||
        json.at("material").get<std::string>().empty())
    {
        error = "detailMaterialInstancePath MI requires non-empty material: " +
            relative;
        return false;
    }
    return true;
}

} // namespace

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
        // registry key、paint sidecar 与离线工具都以正斜杠比较资产路径；
        // 场景/命令行传入的反斜杠在这里统一归一化。
        std::replace(
            desc.assetPath.begin(), desc.assetPath.end(), '\\', '/');
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
        desc.detailMaterialInstancePath =
            asset.value("detailMaterialInstancePath", std::string());
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
                layer.displacementM = layerJson.value("displacementM", 0.0f);
                layer.hasHeight = layerJson.value("hasHeight", false);
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
        const std::string baseMaterialPhysical =
            CommonFunction::Path(desc.materialInstancePath);
        if (!desc.detailMaterialInstancePath.empty())
        {
            std::filesystem::path detailMaterialPhysical;
            std::string detailMaterialError;
            if (!ResolveResourceRootRelativeFile(
                    desc.detailMaterialInstancePath,
                    "detailMaterialInstancePath",
                    detailMaterialPhysical,
                    detailMaterialError))
            {
                throw std::runtime_error(detailMaterialError);
            }
            if (!ValidateDetailMaterialInstanceFile(
                    detailMaterialPhysical,
                    desc.detailMaterialInstancePath,
                    detailMaterialError))
            {
                throw std::runtime_error(detailMaterialError);
            }
            // 两条路径都指向同一份 MI 时，LOD0 与 LOD>0 会重新落回同一材质，
            // 失去“仅近处细分”的收敛意义。这里选择显式拒绝，而不是静默忽略。
            std::error_code equivalentError;
            if (std::filesystem::equivalent(
                    detailMaterialPhysical,
                    std::filesystem::path(baseMaterialPhysical),
                    equivalentError) &&
                !equivalentError)
            {
                throw std::runtime_error(
                    "detailMaterialInstancePath must differ from "
                    "materialInstancePath: " + desc.detailMaterialInstancePath);
            }
        }

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

                // VLP2 sidecar 是层身份的校验记录：存在时必须与刚恢复的
                // paint 形状和当前 TR 的层 key 顺序完全一致，否则宁可失败，
                // 也不能把权重静默套到错误的层上。旧资产没有 sidecar 时只
                // 打印一次 warning 并继续按 VLP1 使用。
                std::string sidecarQueryError;
                if (TerrainPaintManifestExists(
                        paintPhysical, sidecarQueryError))
                {
                    TerrainPaintManifest manifest;
                    std::string manifestError;
                    if (!LoadTerrainPaintManifest(
                            paintPhysical, manifest, manifestError))
                    {
                        throw std::runtime_error(
                            "Terrain paint sidecar load failed: TR=" +
                            pathString + " paint=" +
                            paintPhysical.string() + ": " + manifestError);
                    }
                    std::string validationError;
                    if (!ValidateTerrainPaintManifest(
                            manifest,
                            TerrainPaintLayerKeys(plan.desc),
                            paint->width,
                            paint->height,
                            validationError))
                    {
                        throw std::runtime_error(
                            "Terrain paint sidecar validation failed: TR=" +
                            pathString + " paint=" +
                            paintPhysical.string() + ": " + validationError);
                    }
                }
                else if (!sidecarQueryError.empty())
                {
                    throw std::runtime_error(
                        "Terrain paint sidecar cannot be queried: TR=" +
                        pathString + " paint=" +
                        paintPhysical.string() + ": " +
                        sidecarQueryError);
                }
                else
                {
                    // 兼容没有 sidecar 的旧资产；这条 warning 文案是契约的一部分。
                    // 相邻字面量在编译期拼接，运行期输出仍是一条完整消息。
                    std::cout
                        << "[Terrain] paint sidecar missing (leg"
                           "acy asset): "
                        << paintPhysical.string() << std::endl;
                }
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
