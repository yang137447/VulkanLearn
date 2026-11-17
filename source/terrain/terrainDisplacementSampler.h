#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>

#include "resource/image/hostImage.h"
#include "terrain/terrainCollisionTypes.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainWeightMap.h"

namespace VL
{

// 一张 CPU 高度纹理（归一化 0..1，repeat 采样，显式 mip）。
// 专门服务地形近场位移：GPU 用 sampler2DArray 的 mip 与 repeat 语义，
// CPU 侧必须能按同一 mip 与 wrap 规则取值，脚 IK 才不会和画面差出厘米级。
class TerrainHeightTexture
{
public:
    struct MipLevel
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<float> texels;
    };

    // buildMips=false 只保留 mip0。
    // flipY 与纹理资产 JSON 的 flipY 一致；LDR 路径默认 true。
    static std::shared_ptr<const TerrainHeightTexture> FromHostImage(
        const HostImage& image,
        bool flipY,
        bool buildMips);

    // 显式 mip + repeat + bilinear；mip 越界钳到最粗一级。
    float Sample(float u, float v, uint32_t mip) const;
    uint32_t MipCount() const { return static_cast<uint32_t>(mips.size()); }
    uint32_t Width() const { return mips.empty() ? 0 : mips.front().width; }
    uint32_t Height() const { return mips.empty() ? 0 : mips.front().height; }
    // 只读访问某一级 mip；越界钳到最粗一级（供 parity 参考实现使用）。
    const MipLevel& Mip(uint32_t mip) const;

private:
    std::vector<MipLevel> mips;
};

// 16-bit 高度纹理缓存。同一份层高度数组会被多个 terrain placement 共享，
// 因此解码结果按“绝对路径 + flipY + mip 需求”缓存，避免每个 tile 重新解码。
class TerrainHeightTextureCache
{
public:
    std::shared_ptr<const TerrainHeightTexture> Load(
        const std::filesystem::path& path,
        bool flipY,
        bool buildMips);

    void Clear();
    size_t Size() const { return entries.size(); }

private:
    struct Entry
    {
        std::shared_ptr<const TerrainHeightTexture> texture;
    };
    std::unordered_map<std::string, Entry> entries;
};

struct TerrainDisplacementMaterialParams
{
    // 与 M_terrain schema 默认值一致；detail MI 缺参数时沿用基础 MI 的值。
    float uvScale = 64.0f;
    float detailUvScale = 12.0f;
    float layerDisplacementScale = 1.0f;
    float tessRadiusM = 60.0f;
    // 0 = 每 tile page atlas 的 top-4；1 = 连续 layerWeightMaps。
    // Stage 1 只支持 0；1 在位移开启时明确拒绝，避免静默近似。
    int weightMapMode = 0;
};

// 一个 terrain placement 的 CPU 位移输入。纹理与 MI 参数都来自
// 渲染路径使用的同一份资产（detail MI 优先，缺失时回退基础 MI）。
struct TerrainDisplacementSourceDesc
{
    TerrainBuildPlan plan;
    TerrainCollisionPlacement placement;
    TerrainDisplacementMaterialParams material;
    std::vector<std::shared_ptr<const TerrainHeightTexture>> layerHeights;
};

struct TerrainDisplacementSample
{
    bool valid = false;
    bool insideFootprint = false;
    float heightCm = 0.0f;
    Eigen::Vector3f worldPosition = Eigen::Vector3f::Zero();
    // SampleAtUv 返回宏高度插值法线；SampleWorld 会用位移表面的
    // 20 cm 中心差分法线覆盖它。
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
    std::array<uint8_t, 4> layerIds = {0, 0, 0, 0};
    std::array<float, 4> layerWeights = {1.0f, 0.0f, 0.0f, 0.0f};
    float layerDisplacementCm = 0.0f;
};

// 同源位移采样器：复刻 base.tese.glsl 的几何公式，作为脚 IK / parity 的
// CPU 真值。它只读 asset 数据，不访问 Vulkan 或渲染线程状态。
//
// 与 TES 的对应关系：
// - 宏高度：TerrainHeightmap::SampleHeightCm（mesh UV -> heightmap UV）
// - 权重：ids[0] 主导层；权重来自每 tile page atlas 的 top-4 texel 双线性
// - 层位移：layerHeight LOD2 + repeat + 资产 flipY；幅度 = displacementM *
//   u_layerDisplacementScale，沿插值世界法线
class TerrainDisplacementSampler
{
public:
    explicit TerrainDisplacementSampler(TerrainDisplacementSourceDesc desc);

    // 世界 XZ -> mesh UV。placement 的平移/旋转/缩放与场景对象一致。
    Eigen::Vector2f WorldToUv(float worldX, float worldZ) const;
    Eigen::Vector3f UvToWorld(
        float u,
        float v,
        float heightCm,
        float extraNormalDisplacementCm = 0.0f) const;
    bool ContainsWorldXz(float worldX, float worldZ) const;

    // 视觉高度（cm），包含层位移；不应用相机 near/far fade。
    // 斜坡上按固定 XZ 迭代 1..2 次反解 UV，因为位移沿世界法线会改变 XZ。
    float SampleHeightCm(float worldX, float worldZ) const;
    // 位移表面的平滑几何法线（世界空间，20 cm 中心差分）。
    Eigen::Vector3f SampleNormal(
        float worldX,
        float worldZ,
        float stepCm = 20.0f) const;
    void SampleTop4(
        float u,
        float v,
        std::array<uint8_t, 4>& ids,
        std::array<float, 4>& weights) const;
    TerrainDisplacementSample SampleWorld(
        float worldX,
        float worldZ,
        float normalStepCm = 20.0f) const;
    // 直接按 mesh UV 采样（parity 测试用）；fade 是 TES 的
    // displacementFade 等价物，默认 1（关闭相机淡出）。
    TerrainDisplacementSample SampleAtUv(float u, float v, float fade = 1.0f) const;

    const TerrainDisplacementSourceDesc& Source() const { return source; }

private:
    struct TileWeights
    {
        TerrainTileCandidateList candidates;
    };

    const TileWeights& GetTileWeights(uint32_t tileX, uint32_t tileZ) const;
    TerrainDisplacementSample SampleWorldWithoutNormal(
        float worldX,
        float worldZ,
        int iterations) const;
    float SampleHeightCmAtWorld(
        float worldX,
        float worldZ,
        int iterations) const;
    void SampleTop4Raw(
        float u,
        float v,
        std::array<uint8_t, 4>& ids,
        std::array<float, 4>& weights) const;
    float SampleLayerHeight(
        uint32_t layer,
        float u,
        float v,
        uint32_t mip) const;
    float MacroHeightCm(float u, float v) const;
    Eigen::Vector3f MacroNormalWorld(float u, float v) const;
    float EvaluateDisplacementCm(
        float u,
        float v,
        const std::array<uint8_t, 4>& ids,
        const std::array<float, 4>& weights,
        Eigen::Vector3f& outNormalWorld,
        float& outLayerDisplacementCm) const;

    TerrainDisplacementSourceDesc source;
    Eigen::Matrix3f rotation = Eigen::Matrix3f::Identity();
    Eigen::Matrix3f inverseRotation = Eigen::Matrix3f::Identity();
    Eigen::Vector3f inverseScale = Eigen::Vector3f::Ones();
    TerrainHeightmap::MeshUvTransform heightUvTransform;
    uint32_t tilesPerSide = 1;
    uint32_t pageResolution = 128;
    uint32_t weightResolution = 128;
    mutable std::vector<TileWeights> tileCache;
    mutable std::vector<uint8_t> tileCacheValid;
};

// 从资源根解析 detail/base MI、layerHeight 纹理资产，
// 建立一个 placement 的采样器。失败抛异常；调用方应在 Play 的
// 事务边界内调用，避免留下半套物理/采样状态。
std::shared_ptr<const TerrainDisplacementSampler>
CreateTerrainDisplacementSampler(
    const std::filesystem::path& resourceRoot,
    const TerrainCollisionInstance& instance,
    TerrainHeightTextureCache& textureCache);

// TES 的 displacementFade（far fade * near fade）。CPU 接地查询默认不用它；
// 保留给 parity 与调试，确保能复刻某个相机位置的视觉表面。
float TerrainDisplacementFade(
    const Eigen::Vector3f& cameraPositionCm,
    const Eigen::Vector3f& worldPositionCm,
    float tessRadiusM);

} // namespace VL
