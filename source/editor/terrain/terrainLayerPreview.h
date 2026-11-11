#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "resource/image/hostImage.h"
#include "terrain/terrainAssetTypes.h"

namespace VL::Editor
{

struct TerrainLayerPreviewPaths
{
    std::string spherePath;
    std::string planePath;
};

// 源纹理的时间戳快照。预览 key 用 mtime + size 而不是内容哈希：
// layer source 是 512² PNG，逐次哈希会吞掉编辑器的交互预算；编辑器保存
// 资产会更新 mtime，因此这对字段足以让任何正式源修改换出新缓存。
struct TerrainLayerPreviewSourceStamp
{
    std::filesystem::path path;
    uintmax_t size = 0;
    std::filesystem::file_time_type writeTime{};

    bool IsValid() const noexcept
    {
        return !path.empty();
    }
};

// 16-bit 灰度位移场。保留 PNG 的原始 uint16 采样，只在采样时归一化到 [0,1]；
// 预览链路任何环节都不能先降到 8-bit，否则 0x8000 与 0xFFFF 的细节会直接丢失。
struct TerrainLayerHeightField
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint16_t> samples;

    bool IsValid() const noexcept
    {
        return width > 0 && height > 0 &&
            samples.size() >= static_cast<size_t>(width) * height;
    }

    // repeat + bilinear 采样，和 albedo/normal/pbr 共用同一套 UV/repeat。
    float SampleNormalized(float u, float v) const;
};

// 使用 stbi_load_16 读取单通道 16-bit grayscale，归一化基数固定为 65535。
// 8-bit 输入会被 stb 扩展到 16-bit 范围，但不会经过 8-bit 预览通路。
bool LoadTerrainLayerDisplacement(
    const std::filesystem::path& path,
    TerrainLayerHeightField& output);

struct TerrainLayerPreviewSources
{
    std::filesystem::path albedoPath;
    std::filesystem::path normalPath;
    std::filesystem::path pbrPath;
    std::filesystem::path displacementPath;
    // 组里是否显式声明了 displacementSources；用于区分“旧组没有该字段”
    // 和“声明了但当前 slice 缺失/无法解析”。
    bool displacementDeclared = false;

    bool IsComplete() const noexcept
    {
        return !albedoPath.empty() && !normalPath.empty() && !pbrPath.empty();
    }
};

enum class TerrainLayerPreviewKind
{
    Sphere,
    Plane
};

// 预览参数同时进入缓存 key。每个 source-set 的 albedo textureArray 可携带
// 与 sources 同序的 displacementSources（16-bit grayscale）；只有缺少该字段的
// 旧组才退回 pbr.a（有效 alpha）或 albedo 亮度。位移仅影响预览，不改写运行时高度。
struct TerrainLayerPreviewSettings
{
    uint32_t sphereSize = 96;
    uint32_t planeWidth = 160;
    uint32_t planeHeight = 112;
    uint32_t shadingVersion = 4;
    float textureRepeat = 2.5f;
    float displacementNormalStrength = 7.0f;
    float displacementRadius = 0.06f;
};

inline constexpr TerrainLayerPreviewSettings DefaultTerrainLayerPreviewSettings{};

// RmlUi 层库只消费这两个绝对 PNG 路径；空字符串表示该层没有可解析的
// 完整 source-set albedo/normal/pbr，调用方必须退回 avgColor 程序化色块。
class ITerrainLayerPreviewProvider
{
public:
    virtual ~ITerrainLayerPreviewProvider() = default;
    virtual TerrainLayerPreviewPaths Get(
        const TerrainAssetDesc& desc, size_t layerIndex) = 0;
};

// 返回该层在所属 sourceSet 内的局部 slice：
// 同 sourceSet 的早前层按 TR 顺序占用 0,1,2...；索引越界返回 SIZE_MAX。
size_t TerrainLayerSourceSlice(const TerrainAssetDesc& desc, size_t layerIndex);

// 资源根相对路径解析：MI JSON -> textures.layerAlbedo -> textureArray.sources[slice]。
// 任一环节缺失、类型不匹配、路径越界或源文件不存在时返回空 path。
std::filesystem::path ResolveTerrainLayerPreviewSource(
    const TerrainAssetDesc& desc,
    size_t layerIndex,
    const std::filesystem::path& resourceRoot);

// 同时解析 albedo/normal/pbr 三张 textureArray 的同 slice 源，以及 albedo
// 组可选的 displacementSources 同 slice 16-bit 位移源；只有 albedo 可解析时
// normalPath/pbrPath 为空，调用方按完整 PBR 集缺失处理。
TerrainLayerPreviewSources ResolveTerrainLayerPreviewSources(
    const TerrainAssetDesc& desc,
    size_t layerIndex,
    const std::filesystem::path& resourceRoot);

// 纯 CPU 图像生成；三个输入必须是已解码的 RGBA8 图像（UNORM/SRGB 均可）。
// pbr 通道约定与 M_terrain 一致：R=roughness、G=metallic、B=AO。
// displacement 非空时必须有效并作为唯一高度来源；nullptr 才走旧 pbr.a/亮度回退。
bool BuildTerrainLayerSpherePreview(
    const HostImage& albedo,
    const HostImage& normal,
    const HostImage& pbr,
    const TerrainLayerHeightField* displacement,
    const TerrainLayerPreviewSettings& settings,
    HostImage& output);
bool BuildTerrainLayerPlanePreview(
    const HostImage& albedo,
    const HostImage& normal,
    const HostImage& pbr,
    const TerrainLayerHeightField* displacement,
    const TerrainLayerPreviewSettings& settings,
    HostImage& output);

// 稳定 key：四张源（albedo/normal/pbr/displacement）绝对路径 + 各自大小/修改
// 时间 + 预览尺寸、着色版本与参数 + 预览种类。缺少 displacement 时该 stamp
// 为空，仍参与 key 以区分“旧组回退”与真实 16-bit 源。
std::string TerrainLayerPreviewCacheKey(
    const TerrainLayerPreviewSourceStamp& albedo,
    const TerrainLayerPreviewSourceStamp& normal,
    const TerrainLayerPreviewSourceStamp& pbr,
    const TerrainLayerPreviewSourceStamp& displacement,
    const TerrainLayerPreviewSettings& settings,
    TerrainLayerPreviewKind kind);

class TerrainLayerPreviewCache final : public ITerrainLayerPreviewProvider
{
public:
    TerrainLayerPreviewCache();
    explicit TerrainLayerPreviewCache(std::filesystem::path resourceRoot);

    void SetResourceRoot(std::filesystem::path resourceRoot);
    const std::filesystem::path& ResourceRoot() const noexcept { return resourceRoot; }

    // 清空内存解析/解码结果；磁盘 PNG 仍按源文件时间复用。
    void Reset();

    TerrainLayerPreviewPaths Get(
        const TerrainAssetDesc& desc, size_t layerIndex) override;

private:
    struct TextureArraySource
    {
        std::filesystem::path path;
        std::filesystem::file_time_type writeTime{};
        uintmax_t size = 0;
        std::vector<std::filesystem::path> sources;
        bool displacementDeclared = false;
        std::vector<std::filesystem::path> displacementSources;
    };

    struct MaterialSourceSet
    {
        std::filesystem::path materialPath;
        std::filesystem::file_time_type materialWriteTime{};
        uintmax_t materialSize = 0;
        std::array<TextureArraySource, 3> arrays;
    };

    const MaterialSourceSet* FindMaterialSources(
        const std::string& materialInstancePath);
    std::string EnsurePreview(
        const TerrainLayerPreviewSources& sources,
        const std::array<TerrainLayerPreviewSourceStamp, 4>& stamps,
        std::array<const HostImage*, 3>& decodedSources,
        const TerrainLayerHeightField*& decodedDisplacement,
        TerrainLayerPreviewKind kind);
    const HostImage* DecodeSource(
        const TerrainLayerPreviewSourceStamp& stamp,
        size_t channel);
    const TerrainLayerHeightField* DecodeDisplacement(
        const TerrainLayerPreviewSourceStamp& stamp);
    void ReportFailureOnce(const std::string& key, const std::string& message);

    std::filesystem::path resourceRoot;
    std::unordered_map<std::string, MaterialSourceSet> materialSourceCache;
    std::unordered_map<std::string, HostImage> decodedSourceCache;
    std::unordered_map<std::string, TerrainLayerHeightField>
        decodedDisplacementCache;
    std::unordered_map<std::string, std::string> generatedPathCache;
    std::unordered_set<std::string> failedPreviewKeys;
    std::unordered_set<std::string> reportedFailures;
};

} // namespace VL::Editor
