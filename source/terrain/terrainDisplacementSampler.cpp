#include "terrain/terrainDisplacementSampler.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "resource/image/textureIO.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainWeightMap.h"

namespace VL
{
namespace
{

constexpr float Pi = 3.14159265358979323846f;
constexpr float DegreesToRadians = Pi / 180.0f;

float SmoothStep(float edge0, float edge1, float value)
{
    if (edge1 <= edge0)
        return value >= edge1 ? 1.0f : 0.0f;
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

int WrapIndex(int value, int size)
{
    if (size <= 0)
        return 0;
    int wrapped = value % size;
    if (wrapped < 0)
        wrapped += size;
    return wrapped;
}

float ReadFloatParameter(
    const nlohmann::json& parameters,
    const char* name,
    float fallback)
{
    if (!parameters.is_object() || !parameters.contains(name))
        return fallback;
    const nlohmann::json& value = parameters.at(name);
    if (!value.is_number())
        throw std::runtime_error(
            std::string("Terrain displacement material parameter requires a "
                        "number: ") + name);
    const float result = value.get<float>();
    if (!std::isfinite(result))
        throw std::runtime_error(
            std::string("Terrain displacement material parameter is not "
                        "finite: ") + name);
    return result;
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream.is_open())
        throw std::runtime_error(
            "Terrain displacement asset is missing: " + path.string());
    nlohmann::json json;
    stream >> json;
    return json;
}

std::filesystem::path ResolveAsset(
    const std::filesystem::path& resourceRoot,
    const std::string& assetPath)
{
    if (assetPath.empty())
        throw std::runtime_error("Terrain displacement asset path is empty");
    return resourceRoot / std::filesystem::path(assetPath);
}

struct MaterialInstanceTextures
{
    std::string layerHeight;
    std::string detailHeightMap;
};

MaterialInstanceTextures ReadMaterialInstanceTextures(
    const nlohmann::json& json,
    const std::filesystem::path& path)
{
    MaterialInstanceTextures textures;
    if (!json.contains("textures"))
        return textures;
    const nlohmann::json& source = json.at("textures");
    if (!source.is_object())
        throw std::runtime_error(
            "Terrain displacement material instance textures must be an "
            "object: " + path.string());
    if (source.contains("layerHeight"))
    {
        if (!source.at("layerHeight").is_string())
            throw std::runtime_error(
                "Terrain layerHeight texture must be a string: " +
                path.string());
        textures.layerHeight = source.at("layerHeight").get<std::string>();
    }
    if (source.contains("detailHeightMap"))
    {
        if (!source.at("detailHeightMap").is_string())
            throw std::runtime_error(
                "Terrain detailHeightMap texture must be a string: " +
                path.string());
        textures.detailHeightMap =
            source.at("detailHeightMap").get<std::string>();
    }
    return textures;
}

// Texture array assets list their per-slice sources; layerHeight keeps the
// engine's default flipY=true unless the asset overrides it.
std::shared_ptr<const TerrainHeightTexture> LoadTextureArraySlice(
    const std::filesystem::path& resourceRoot,
    const nlohmann::json& arrayJson,
    const std::string& assetPath,
    size_t slice,
    TerrainHeightTextureCache& cache,
    uint32_t expectedSliceCount)
{
    if (!arrayJson.contains("type") ||
        arrayJson.at("type").get<std::string>() != "textureArray")
        throw std::runtime_error(
            "Terrain layerHeight must reference a textureArray asset: " +
            assetPath);
    if (!arrayJson.contains("sources") ||
        !arrayJson.at("sources").is_array())
        throw std::runtime_error(
            "Terrain layerHeight textureArray has no sources: " + assetPath);
    const size_t sourceCount = arrayJson.at("sources").size();
    if (sourceCount < expectedSliceCount)
        throw std::runtime_error(
            "Terrain layerHeight textureArray has " +
            std::to_string(sourceCount) + " slices but the TR declares " +
            std::to_string(expectedSliceCount) + ": " + assetPath);
    if (slice >= sourceCount)
        throw std::runtime_error(
            "Terrain layerHeight slice is outside the textureArray: " +
            assetPath);
    if (!arrayJson.at("sources")[slice].is_string())
        throw std::runtime_error(
            "Terrain layerHeight textureArray source must be a string: " +
            assetPath);
    const std::string source =
        arrayJson.at("sources")[slice].get<std::string>();
    const bool flipY = arrayJson.value("flipY", true);
    return cache.Load(ResolveAsset(resourceRoot, source), flipY, true);
}

std::shared_ptr<const TerrainHeightTexture> LoadDetailHeight(
    const std::filesystem::path& resourceRoot,
    const std::string& assetPath,
    TerrainHeightTextureCache& cache)
{
    const std::filesystem::path path =
        ResolveAsset(resourceRoot, assetPath);
    const nlohmann::json json = ReadJsonFile(path);
    if (!json.contains("source") || !json.at("source").is_string())
        throw std::runtime_error(
            "Terrain detailHeightMap has no source: " + assetPath);
    const std::string source = json.at("source").get<std::string>();
    // TES samples detailHeightMap with explicit LOD0; building a CPU mip chain
    // would only waste memory.
    const bool flipY = json.value("flipY", true);
    return cache.Load(ResolveAsset(resourceRoot, source), flipY, false);
}

Eigen::Matrix3f PlacementRotationMatrix(const Eigen::Vector3f& degrees)
{
    const Eigen::Vector3f radians = degrees * DegreesToRadians;
    const Eigen::AngleAxisf yaw(radians.y(), Eigen::Vector3f::UnitY());
    const Eigen::AngleAxisf pitch(radians.x(), Eigen::Vector3f::UnitX());
    const Eigen::AngleAxisf roll(radians.z(), Eigen::Vector3f::UnitZ());
    return (yaw * pitch * roll).toRotationMatrix();
}

void ValidatePlacement(const TerrainBuildPlan& plan, const TerrainCollisionPlacement& placement)
{
    if (!plan.heightmap)
        throw std::runtime_error("Terrain displacement sampler requires a decoded heightmap");
    if (!(plan.desc.size.x() > 0.0f) || !(plan.desc.size.y() > 0.0f) ||
        !std::isfinite(plan.desc.size.x()) || !std::isfinite(plan.desc.size.y()))
        throw std::runtime_error("Terrain displacement sampler requires positive finite size");
    const auto finite = [](const Eigen::Vector3f& value)
    {
        return std::isfinite(value.x()) && std::isfinite(value.y()) &&
            std::isfinite(value.z());
    };
    if (!finite(placement.position) || !finite(placement.rotation) ||
        !finite(placement.scale) || placement.scale.x() <= 0.0f ||
        placement.scale.y() <= 0.0f || placement.scale.z() <= 0.0f)
        throw std::runtime_error(
            "Terrain displacement sampler requires finite placement and "
            "positive scale");
}

} // namespace

std::shared_ptr<const TerrainHeightTexture> TerrainHeightTexture::FromHostImage(
    const HostImage& image,
    bool flipY,
    bool buildMips)
{
    if (image.width == 0 || image.height == 0 || image.data.empty())
        throw std::runtime_error("Terrain height texture is empty");

    MipLevel base;
    base.width = image.width;
    base.height = image.height;
    base.texels.resize(
        static_cast<size_t>(image.width) * image.height);
    for (uint32_t y = 0; y < image.height; ++y)
    {
        const uint32_t sourceY = flipY ? image.height - 1 - y : y;
        for (uint32_t x = 0; x < image.width; ++x)
        {
            const float value = TerrainHeightmap::ReadNormalizedPixel(
                image, x, sourceY);
            if (!std::isfinite(value))
                throw std::runtime_error(
                    "Terrain height texture contains non-finite samples");
            base.texels[static_cast<size_t>(y) * image.width + x] = value;
        }
    }

    auto texture = std::make_shared<TerrainHeightTexture>();
    texture->mips.push_back(std::move(base));
    if (buildMips)
    {
        while (texture->mips.back().width > 1 ||
            texture->mips.back().height > 1)
        {
            const MipLevel& previous = texture->mips.back();
            MipLevel next;
            next.width = std::max(1u, previous.width / 2);
            next.height = std::max(1u, previous.height / 2);
            next.texels.resize(
                static_cast<size_t>(next.width) * next.height);
            for (uint32_t y = 0; y < next.height; ++y)
            {
                for (uint32_t x = 0; x < next.width; ++x)
                {
                    const uint32_t x0 = std::min(previous.width - 1, x * 2);
                    const uint32_t x1 = std::min(previous.width - 1, x * 2 + 1);
                    const uint32_t y0 = std::min(previous.height - 1, y * 2);
                    const uint32_t y1 = std::min(previous.height - 1, y * 2 + 1);
                    const float sum =
                        previous.texels[static_cast<size_t>(y0) * previous.width + x0] +
                        previous.texels[static_cast<size_t>(y0) * previous.width + x1] +
                        previous.texels[static_cast<size_t>(y1) * previous.width + x0] +
                        previous.texels[static_cast<size_t>(y1) * previous.width + x1];
                    next.texels[static_cast<size_t>(y) * next.width + x] =
                        sum * 0.25f;
                }
            }
            texture->mips.push_back(std::move(next));
        }
    }
    return texture;
}

float TerrainHeightTexture::Sample(float u, float v, uint32_t mip) const
{
    if (mips.empty())
        return 0.5f;
    const MipLevel& level = mips[std::min<size_t>(mip, mips.size() - 1)];
    if (level.width == 0 || level.height == 0 || level.texels.empty())
        return 0.5f;

    const float x = u * static_cast<float>(level.width) - 0.5f;
    const float y = v * static_cast<float>(level.height) - 0.5f;
    const float floorX = std::floor(x);
    const float floorY = std::floor(y);
    const int x0 = static_cast<int>(floorX);
    const int y0 = static_cast<int>(floorY);
    const float fx = x - floorX;
    const float fy = y - floorY;
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const auto index = [&level](int px, int py)
    {
        const int wrappedX = WrapIndex(px, static_cast<int>(level.width));
        const int wrappedY = WrapIndex(py, static_cast<int>(level.height));
        return level.texels[
            static_cast<size_t>(wrappedY) * level.width + wrappedX];
    };
    const float top = index(x0, y0) + (index(x1, y0) - index(x0, y0)) * fx;
    const float bottom = index(x0, y1) + (index(x1, y1) - index(x0, y1)) * fx;
    return top + (bottom - top) * fy;
}

const TerrainHeightTexture::MipLevel& TerrainHeightTexture::Mip(
    uint32_t mip) const
{
    static const MipLevel empty;
    if (mips.empty())
        return empty;
    return mips[std::min<size_t>(mip, mips.size() - 1)];
}

std::shared_ptr<const TerrainHeightTexture> TerrainHeightTextureCache::Load(
    const std::filesystem::path& path,
    bool flipY,
    bool buildMips)
{
    const std::string key =
        path.lexically_normal().generic_string() +
        (flipY ? "|flipY" : "|noFlip") +
        (buildMips ? "|mips" : "|mip0");
    const auto existing = entries.find(key);
    if (existing != entries.end())
        return existing->second.texture;

    TextureIO::LoadOptions options;
    options.semantic = HostImage::TextureSemantic::Data;
    options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
    options.transfer = TextureIO::LoadOptions::Transfer::Linear;
    options.forceChannels = 1;
    options.forceSixteenBit = true;
    const std::optional<HostImage> image = TextureIO::Load(path, options);
    if (!image)
        throw std::runtime_error(
            "Terrain displacement texture decode failed: " + path.string());

    std::shared_ptr<const TerrainHeightTexture> texture =
        TerrainHeightTexture::FromHostImage(*image, flipY, buildMips);
    entries.emplace(key, Entry{texture});
    return texture;
}

void TerrainHeightTextureCache::Clear()
{
    entries.clear();
}

TerrainDisplacementSampler::TerrainDisplacementSampler(
    TerrainDisplacementSourceDesc desc)
    : source(std::move(desc))
{
    ValidatePlacement(source.plan, source.placement);
    heightUvTransform = TerrainHeightmap::MeshUvTransformFor(
        source.plan.desc, *source.plan.heightmap);
    rotation = PlacementRotationMatrix(source.placement.rotation);
    inverseRotation = rotation.transpose();
    inverseScale = source.placement.scale.cwiseInverse();
    tilesPerSide = std::max(1u, source.plan.desc.pageTilesPerSide);
    pageResolution = TerrainPageResolution;
    weightResolution = tilesPerSide * pageResolution;
    tileCache.resize(static_cast<size_t>(tilesPerSide) * tilesPerSide);
    tileCacheValid.assign(tileCache.size(), 0);
    for (uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            TileWeights& tile =
                tileCache[static_cast<size_t>(tileZ) * tilesPerSide + tileX];
            tile.candidates.tileX = tileX;
            tile.candidates.tileZ = tileZ;
        }
    }
}

Eigen::Vector2f TerrainDisplacementSampler::WorldToUv(
    float worldX,
    float worldZ) const
{
    const Eigen::Vector3f worldDelta(
        worldX - source.placement.position.x(),
        0.0f,
        worldZ - source.placement.position.z());
    const Eigen::Vector3f local =
        inverseScale.cwiseProduct(inverseRotation * worldDelta);
    return Eigen::Vector2f(
        local.x() / source.plan.desc.size.x() + 0.5f,
        local.z() / source.plan.desc.size.y() + 0.5f);
}

Eigen::Vector3f TerrainDisplacementSampler::UvToWorld(
    float u,
    float v,
    float heightCm,
    float extraNormalDisplacementCm) const
{
    const Eigen::Vector3f local(
        (u - 0.5f) * source.plan.desc.size.x(),
        heightCm,
        (v - 0.5f) * source.plan.desc.size.y());
    Eigen::Vector3f world = source.placement.position +
        rotation * source.placement.scale.cwiseProduct(local);
    if (extraNormalDisplacementCm != 0.0f)
        world += MacroNormalWorld(u, v) * extraNormalDisplacementCm;
    return world;
}

bool TerrainDisplacementSampler::ContainsWorldXz(
    float worldX,
    float worldZ) const
{
    const Eigen::Vector3f worldDelta(
        worldX - source.placement.position.x(),
        0.0f,
        worldZ - source.placement.position.z());
    const Eigen::Vector3f local =
        inverseScale.cwiseProduct(inverseRotation * worldDelta);
    const float halfX = source.plan.desc.size.x() * 0.5f;
    const float halfZ = source.plan.desc.size.y() * 0.5f;
    return std::abs(local.x()) <= halfX + 1.0f &&
        std::abs(local.z()) <= halfZ + 1.0f;
}

float TerrainDisplacementSampler::MacroHeightCm(float u, float v) const
{
    const TerrainAssetDesc& desc = source.plan.desc;
    const Eigen::Vector2f heightUv = heightUvTransform.Apply(u, v);
    return TerrainHeightmap::SampleHeightCm(
        desc, *source.plan.heightmap, heightUv.x(), heightUv.y());
}

Eigen::Vector3f TerrainDisplacementSampler::MacroNormalWorld(
    float u,
    float v) const
{
    const TerrainAssetDesc& desc = source.plan.desc;
    const float resolution = static_cast<float>(
        std::max(1u, desc.gridResolution));
    const float du = 1.0f / resolution;
    const float left = MacroHeightCm(u - du, v);
    const float right = MacroHeightCm(u + du, v);
    const float down = MacroHeightCm(u, v - du);
    const float up = MacroHeightCm(u, v + du);
    const float worldDx = 2.0f * du * desc.size.x();
    const float worldDz = 2.0f * du * desc.size.y();
    const Eigen::Vector3f localNormal(
        -(right - left) / worldDx,
        1.0f,
        -(up - down) / worldDz);
    return (rotation * inverseScale.cwiseProduct(localNormal)).normalized();
}

const TerrainDisplacementSampler::TileWeights&
TerrainDisplacementSampler::GetTileWeights(
    uint32_t tileX,
    uint32_t tileZ) const
{
    const size_t index = static_cast<size_t>(tileZ) * tilesPerSide + tileX;
    TileWeights& tile = tileCache[index];
    if (tileCacheValid[index] == 0)
    {
        tile.candidates = TerrainWeightMapBuilder::BuildPageCandidates(
            source.plan,
            tilesPerSide,
            pageResolution,
            tileX,
            tileZ);
        tileCacheValid[index] = 1;
    }
    return tile;
}

void TerrainDisplacementSampler::SampleTop4Raw(
    float u,
    float v,
    std::array<uint8_t, 4>& ids,
    std::array<float, 4>& weights) const
{
    if (source.plan.desc.layers.empty())
    {
        ids = {0, 0, 0, 0};
        weights = {1.0f, 0.0f, 0.0f, 0.0f};
        return;
    }
    const float clampedU = std::clamp(u, 0.0f, 1.0f);
    const float clampedV = std::clamp(v, 0.0f, 1.0f);
    const float scaledU = clampedU * static_cast<float>(tilesPerSide);
    const float scaledV = clampedV * static_cast<float>(tilesPerSide);
    uint32_t tileX = static_cast<uint32_t>(std::max(0.0f, std::floor(scaledU)));
    uint32_t tileZ = static_cast<uint32_t>(std::max(0.0f, std::floor(scaledV)));
    tileX = std::min(tileX, tilesPerSide - 1);
    tileZ = std::min(tileZ, tilesPerSide - 1);
    float localU = scaledU - static_cast<float>(tileX);
    float localV = scaledV - static_cast<float>(tileZ);
    if (clampedU >= 1.0f)
        localU = 1.0f;
    if (clampedV >= 1.0f)
        localV = 1.0f;
    const float halfTexel = 0.5f / static_cast<float>(pageResolution);
    localU = std::clamp(localU, halfTexel, 1.0f - halfTexel);
    localV = std::clamp(localV, halfTexel, 1.0f - halfTexel);

    const float texelX = localU * static_cast<float>(pageResolution) - 0.5f;
    const float texelY = localV * static_cast<float>(pageResolution) - 0.5f;
    const float floorX = std::floor(texelX);
    const float floorY = std::floor(texelY);
    const float fx = texelX - floorX;
    const float fy = texelY - floorY;
    const int ix0 = std::clamp(
        static_cast<int>(floorX), 0, static_cast<int>(pageResolution) - 1);
    const int iy0 = std::clamp(
        static_cast<int>(floorY), 0, static_cast<int>(pageResolution) - 1);
    const int ix1 = std::min(ix0 + 1, static_cast<int>(pageResolution) - 1);
    const int iy1 = std::min(iy0 + 1, static_cast<int>(pageResolution) - 1);

    const TileWeights& tile = GetTileWeights(tileX, tileZ);
    const TerrainTileCandidateList& candidates = tile.candidates;

    const TerrainTop4Texel samples[4] = {
        TerrainWeightMapBuilder::SampleTop4Texel(
            source.plan, weightResolution, candidates,
            tileX * pageResolution + static_cast<uint32_t>(ix0),
            tileZ * pageResolution + static_cast<uint32_t>(iy0)),
        TerrainWeightMapBuilder::SampleTop4Texel(
            source.plan, weightResolution, candidates,
            tileX * pageResolution + static_cast<uint32_t>(ix1),
            tileZ * pageResolution + static_cast<uint32_t>(iy0)),
        TerrainWeightMapBuilder::SampleTop4Texel(
            source.plan, weightResolution, candidates,
            tileX * pageResolution + static_cast<uint32_t>(ix0),
            tileZ * pageResolution + static_cast<uint32_t>(iy1)),
        TerrainWeightMapBuilder::SampleTop4Texel(
            source.plan, weightResolution, candidates,
            tileX * pageResolution + static_cast<uint32_t>(ix1),
            tileZ * pageResolution + static_cast<uint32_t>(iy1))};
    const float bilinear[4] = {
        (1.0f - fx) * (1.0f - fy),
        fx * (1.0f - fy),
        (1.0f - fx) * fy,
        fx * fy};

    float weightBlend[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (size_t sample = 0; sample < 4; ++sample)
    {
        for (uint32_t slot = 0; slot < 4; ++slot)
        {
            weightBlend[slot] += bilinear[sample] *
                static_cast<float>(samples[sample].weights[slot]) / 255.0f;
        }
    }
    // layerIds atlas 是 vk::Filter::eNearest（terrainAtlasGpu.cpp），
    // 只有 layerWeights 是 linear。ID 必须取最近的 texel，不能做双线性，
    // 否则层号会在过渡带被插值成不存在的中间层。
    const size_t idSample =
        (fx < 0.5f ? 0u : 1u) + (fy < 0.5f ? 0u : 2u);
    for (uint32_t slot = 0; slot < 4; ++slot)
    {
        ids[slot] = samples[idSample].ids[slot];
        weights[slot] = weightBlend[slot];
    }
    float sum = weights[0] + weights[1] + weights[2] + weights[3];
    if (sum < 1.0e-4f)
    {
        weights = {1.0f, 0.0f, 0.0f, 0.0f};
        return;
    }
    for (float& weight : weights)
        weight /= sum;
}

void TerrainDisplacementSampler::SampleTop4(
    float u,
    float v,
    std::array<uint8_t, 4>& ids,
    std::array<float, 4>& weights) const
{
    SampleTop4Raw(u, v, ids, weights);
}

float TerrainDisplacementSampler::SampleLayerHeight(
    uint32_t layer,
    float u,
    float v,
    uint32_t mip) const
{
    if (layer >= source.layerHeights.size() ||
        !source.layerHeights[layer])
        return 0.5f;
    return source.layerHeights[layer]->Sample(u, v, mip);
}

float TerrainDisplacementSampler::SampleDetailHeight(float u, float v) const
{
    if (!source.detailHeight)
        return 0.5f;
    return source.detailHeight->Sample(u, v, 0);
}

float TerrainDisplacementSampler::EvaluateDisplacementCm(
    float u,
    float v,
    const std::array<uint8_t, 4>& ids,
    const std::array<float, 4>& weights,
    Eigen::Vector3f& outNormalWorld,
    float& outLayerDisplacementCm,
    float& outDetailDisplacementCm) const
{
    outNormalWorld = MacroNormalWorld(u, v);
    outLayerDisplacementCm = 0.0f;
    outDetailDisplacementCm = 0.0f;
    if (weights[0] <= 1.0e-5f)
        return 0.0f;
    const TerrainDisplacementMaterialParams& material = source.material;
    const uint32_t layer = ids[0];
    if (layer >= source.plan.desc.layers.size())
        return 0.0f;
    const TerrainLayerDesc& layerDesc = source.plan.desc.layers[layer];
    const bool layerActive =
        std::abs(material.layerDisplacementScale) > 1.0e-4f;
    const bool detailActive =
        std::abs(material.detailAmplitudeCm) > 0.1f;
    if (!layerActive && !detailActive)
        return 0.0f;

    if (layerActive)
    {
        // TES 的主导层位移 UV 不做 rotation：texCoord * u_uvScale *
        // layerUvScale * u_detailUvScale。
        const float uvScale =
            material.uvScale * layerDesc.uvScale * material.detailUvScale;
        const float height = SampleLayerHeight(
            layer, u * uvScale, v * uvScale, 2);
        outLayerDisplacementCm = (height - 0.5f) *
            layerDesc.displacementM * material.layerDisplacementScale * 100.0f;
    }

    // 水/海层 displacementM=0：detail tiling 高度必须保持宏高度平整。
    const float dominantDisplacementM = layerDesc.displacementM;
    const float detailSolidity =
        SmoothStep(0.004f, 0.03f, dominantDisplacementM);
    if (detailActive && detailSolidity > 0.0f)
    {
        const float uvScale = material.uvScale * material.detailUvScale;
        const float height = SampleDetailHeight(u * uvScale, v * uvScale);
        outDetailDisplacementCm =
            (height - 0.5f) * material.detailAmplitudeCm * detailSolidity;
    }
    return outLayerDisplacementCm + outDetailDisplacementCm;
}

TerrainDisplacementSample TerrainDisplacementSampler::SampleAtUv(
    float u,
    float v,
    float fade) const
{
    TerrainDisplacementSample sample;
    sample.valid = std::isfinite(u) && std::isfinite(v);
    sample.insideFootprint =
        u >= -1.0e-4f && u <= 1.0f + 1.0e-4f &&
        v >= -1.0e-4f && v <= 1.0f + 1.0e-4f;
    const float clampedU = std::clamp(u, 0.0f, 1.0f);
    const float clampedV = std::clamp(v, 0.0f, 1.0f);
    const float macroHeight = MacroHeightCm(clampedU, clampedV);
    SampleTop4Raw(clampedU, clampedV, sample.layerIds, sample.layerWeights);
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
    const float displacement = EvaluateDisplacementCm(
        clampedU,
        clampedV,
        sample.layerIds,
        sample.layerWeights,
        normal,
        sample.layerDisplacementCm,
        sample.detailDisplacementCm);
    const Eigen::Vector3f baseWorld = UvToWorld(
        clampedU, clampedV, macroHeight);
    sample.worldPosition = baseWorld + normal * displacement * fade;
    sample.heightCm = sample.worldPosition.y();
    sample.normal = normal;
    return sample;
}

TerrainDisplacementSample TerrainDisplacementSampler::SampleWorldWithoutNormal(
    float worldX,
    float worldZ,
    int iterations) const
{
    TerrainDisplacementSample result;
    if (!std::isfinite(worldX) || !std::isfinite(worldZ))
        return result;
    Eigen::Vector2f uv = WorldToUv(worldX, worldZ);
    const bool inside = ContainsWorldXz(worldX, worldZ);
    // 位移沿世界法线，会推动表面在 XZ 上的位置；固定 XZ 反解 UV，
    // 1..2 次已足够收敛（位移梯度远小于 1）。
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        const TerrainDisplacementSample current = SampleAtUv(uv.x(), uv.y());
        const Eigen::Vector3f delta(
            worldX - current.worldPosition.x(),
            0.0f,
            worldZ - current.worldPosition.z());
        const Eigen::Vector3f localDelta =
            inverseScale.cwiseProduct(inverseRotation * delta);
        uv.x() += localDelta.x() / source.plan.desc.size.x();
        uv.y() += localDelta.z() / source.plan.desc.size.y();
        uv.x() = std::clamp(uv.x(), -0.01f, 1.01f);
        uv.y() = std::clamp(uv.y(), -0.01f, 1.01f);
    }
    result = SampleAtUv(uv.x(), uv.y());
    result.insideFootprint = inside;
    return result;
}

TerrainDisplacementSample TerrainDisplacementSampler::SampleWorld(
    float worldX,
    float worldZ,
    float normalStepCm) const
{
    TerrainDisplacementSample result =
        SampleWorldWithoutNormal(worldX, worldZ, 2);
    if (result.valid)
        result.normal = SampleNormal(worldX, worldZ, normalStepCm);
    return result;
}

float TerrainDisplacementSampler::SampleHeightCm(
    float worldX,
    float worldZ) const
{
    return SampleHeightCmAtWorld(worldX, worldZ, 2);
}

float TerrainDisplacementSampler::SampleHeightCmAtWorld(
    float worldX,
    float worldZ,
    int iterations) const
{
    return SampleWorldWithoutNormal(worldX, worldZ, iterations).heightCm;
}

Eigen::Vector3f TerrainDisplacementSampler::SampleNormal(
    float worldX,
    float worldZ,
    float stepCm) const
{
    const float step = stepCm > 0.0f ? stepCm : 1.0f;
    // 法线只用于平滑接地平面，1 次固定点迭代的亚厘米 UV 残差可以忽略；
    // 这样每次足部法线只需 4 条 1-iteration 高度查询。
    const float left = SampleHeightCmAtWorld(worldX - step, worldZ, 1);
    const float right = SampleHeightCmAtWorld(worldX + step, worldZ, 1);
    const float down = SampleHeightCmAtWorld(worldX, worldZ - step, 1);
    const float up = SampleHeightCmAtWorld(worldX, worldZ + step, 1);
    const Eigen::Vector3f normal(
        -(right - left) / (2.0f * step),
        1.0f,
        -(up - down) / (2.0f * step));
    if (!normal.allFinite() || normal.squaredNorm() < 1.0e-12f)
        return Eigen::Vector3f::UnitY();
    return normal.normalized();
}

float TerrainDisplacementFade(
    const Eigen::Vector3f& cameraPositionCm,
    const Eigen::Vector3f& worldPositionCm,
    float tessRadiusM)
{
    const float radiusCm = std::max(tessRadiusM, 0.0f) * 100.0f;
    const float distanceCm =
        (worldPositionCm - cameraPositionCm).norm();
    const float farFade = radiusCm <= 1.0e-3f
        ? 0.0f
        : 1.0f - SmoothStep(0.75f * radiusCm, radiusCm, distanceCm);
    const float nearFade = SmoothStep(0.0f, 200.0f, distanceCm);
    return farFade * nearFade;
}

std::shared_ptr<const TerrainDisplacementSampler>
CreateTerrainDisplacementSampler(
    const std::filesystem::path& resourceRoot,
    const TerrainCollisionInstance& instance,
    TerrainHeightTextureCache& textureCache)
{
    const TerrainAssetDesc& desc = instance.plan.desc;
    if (resourceRoot.empty())
        throw std::runtime_error(
            "Terrain displacement sampling requires a resource root");

    TerrainDisplacementMaterialParams material;
    MaterialInstanceTextures textures;
    if (!desc.materialInstancePath.empty())
    {
        const nlohmann::json baseJson = ReadJsonFile(
            ResolveAsset(resourceRoot, desc.materialInstancePath));
        const nlohmann::json parameters = baseJson.contains("parameters")
            ? baseJson.at("parameters")
            : nlohmann::json::object();
        material.uvScale = ReadFloatParameter(
            parameters, "u_uvScale", material.uvScale);
        material.detailUvScale = ReadFloatParameter(
            parameters, "u_detailUvScale", material.detailUvScale);
        material.detailAmplitudeCm = ReadFloatParameter(
            parameters, "u_detailAmplitudeCm", material.detailAmplitudeCm);
        material.layerDisplacementScale = ReadFloatParameter(
            parameters,
            "u_layerDisplacementScale",
            material.layerDisplacementScale);
        material.detailNormalStrength = ReadFloatParameter(
            parameters,
            "u_detailNormalStrength",
            material.detailNormalStrength);
        material.tessRadiusM = ReadFloatParameter(
            parameters, "u_terrainTessRadiusM", material.tessRadiusM);
        material.weightMapMode = static_cast<int>(std::lround(
            ReadFloatParameter(
                parameters, "u_terrainWeightMapMode", 0.0f)));
        textures = ReadMaterialInstanceTextures(
            baseJson, ResolveAsset(resourceRoot, desc.materialInstancePath));
    }
    if (!desc.detailMaterialInstancePath.empty())
    {
        const std::filesystem::path detailPath =
            ResolveAsset(resourceRoot, desc.detailMaterialInstancePath);
        const nlohmann::json detailJson = ReadJsonFile(detailPath);
        const nlohmann::json parameters = detailJson.contains("parameters")
            ? detailJson.at("parameters")
            : nlohmann::json::object();
        // RendererTerrainLoader 把决定 VT/UV 语义的参数从基础 MI 同步到
        // detail MI；这里以 detail MI 的显式值为准，缺失时沿用基础值。
        material.uvScale = ReadFloatParameter(
            parameters, "u_uvScale", material.uvScale);
        material.detailUvScale = ReadFloatParameter(
            parameters, "u_detailUvScale", material.detailUvScale);
        material.detailAmplitudeCm = ReadFloatParameter(
            parameters, "u_detailAmplitudeCm", material.detailAmplitudeCm);
        material.layerDisplacementScale = ReadFloatParameter(
            parameters,
            "u_layerDisplacementScale",
            material.layerDisplacementScale);
        material.detailNormalStrength = ReadFloatParameter(
            parameters,
            "u_detailNormalStrength",
            material.detailNormalStrength);
        material.tessRadiusM = ReadFloatParameter(
            parameters, "u_terrainTessRadiusM", material.tessRadiusM);
        const MaterialInstanceTextures detailTextures =
            ReadMaterialInstanceTextures(detailJson, detailPath);
        if (!detailTextures.layerHeight.empty())
            textures.layerHeight = detailTextures.layerHeight;
        if (!detailTextures.detailHeightMap.empty())
            textures.detailHeightMap = detailTextures.detailHeightMap;
    }

    const bool hasLayerDisplacement =
        std::abs(material.layerDisplacementScale) > 1.0e-4f &&
        std::any_of(
            desc.layers.begin(),
            desc.layers.end(),
            [](const TerrainLayerDesc& layer)
            {
                return std::abs(layer.displacementM) > 1.0e-6f;
            });
    const bool hasDetailDisplacement =
        std::abs(material.detailAmplitudeCm) > 0.1f;
    if (material.weightMapMode == 1 &&
        (hasLayerDisplacement || hasDetailDisplacement))
    {
        throw std::runtime_error(
            "Terrain CPU displacement sampling does not support "
            "u_terrainWeightMapMode=1 yet; use the baked page-atlas weights "
            "(mode 0) or disable displacement: " + desc.assetPath);
    }

    TerrainDisplacementSourceDesc source;
    source.plan = instance.plan;
    source.placement = instance.placement;
    source.material = material;

    if (!textures.layerHeight.empty())
    {
        const std::filesystem::path path =
            ResolveAsset(resourceRoot, textures.layerHeight);
        const nlohmann::json json = ReadJsonFile(path);
        source.layerHeights.reserve(desc.layers.size());
        for (size_t layer = 0; layer < desc.layers.size(); ++layer)
        {
            source.layerHeights.push_back(LoadTextureArraySlice(
                resourceRoot,
                json,
                textures.layerHeight,
                layer,
                textureCache,
                desc.layers.size()));
        }
    }
    if (!textures.detailHeightMap.empty())
    {
        source.detailHeight = LoadDetailHeight(
            resourceRoot, textures.detailHeightMap, textureCache);
    }
    return std::make_shared<const TerrainDisplacementSampler>(
        std::move(source));
}

} // namespace VL
