#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

class MaterialInstance;

namespace VL
{

class SubsurfaceResourceSet;

// Material instance loader 根据候选 World resource set 解析出的稳定 ID，随后
// 重新注入 MaterialInstance；不读取文件，也不创建纹理或 descriptor。
struct ResolvedSubsurfaceMaterialAssets
{
    std::optional<uint32_t> profileId;
    std::optional<uint32_t> skinLutId;
};

// 将 MI 中的 profile/LUT 资产路径解析成引擎拥有的稳定数值 ID，并在加载期一次性
// 校验模型专用范围，避免 shader 每帧为错误数据执行防御性修正。
ResolvedSubsurfaceMaterialAssets ResolveSubsurfaceMaterialContract(
    const nlohmann::json& materialInstanceJson,
    nlohmann::json& effectiveMaterialInstanceJson,
    const SubsurfaceResourceSet& resourceSet,
    std::string_view materialInstancePath);

void ReapplyResolvedSubsurfaceMaterialIds(
    const ResolvedSubsurfaceMaterialAssets& resolvedAssets,
    ::MaterialInstance& materialInstance);

} // namespace VL
