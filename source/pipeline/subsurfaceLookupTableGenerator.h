#pragma once

#include <memory>
#include <vector>

#include "render/subsurface/subsurfaceAssets.h"

class PipelineFactory;
class Texture;

namespace VL
{

class RendererBackendVulkan;

// 一次 candidate Compute dispatch 生成的两张 lookup texture，交由 World-local
// SubsurfaceResourceSet 持有；不保留 generator pipeline 或临时 descriptor 资源。
struct SubsurfaceLookupTableGenerationResult
{
    std::shared_ptr<Texture> profileTableTexture;
    std::shared_ptr<Texture> skinLutTableTexture;
};

// 将已验证的 profile/LUT 资产转换为不可变 GPU lookup texture。
// CPU 只序列化作者参数，kernel 权重、归一化和 LUT response 全部由
// generator/subsurfaceLookupTables.comp 计算，不保留 CPU 数值生成回退。
class SubsurfaceLookupTableGenerator
{
public:
    static SubsurfaceLookupTableGenerationResult Generate(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const std::vector<SubsurfaceProfileAsset>& profiles,
        const std::vector<PreintegratedSkinLutAsset>& skinLuts,
        const std::string& sourceDigest);
};

} // namespace VL
