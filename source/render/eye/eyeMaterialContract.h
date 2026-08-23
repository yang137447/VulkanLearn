#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

class MaterialInstance;

namespace VL
{

class EyeResourceSet;
class SubsurfaceResourceSet;

// Material loader 解析出的 Eye 稳定 ID。值会在热重载 snapshot 恢复后再次注入，
// 因此 MI 不能通过旧 generation 的参数缓存住 profile identity。
struct ResolvedEyeMaterialAssets
{
    std::optional<uint32_t> profileId;
    std::optional<uint32_t> scleraProfileId;
    float corneaIor = 1.376f;
    float causticStrength = 0.0f;
};

ResolvedEyeMaterialAssets ResolveEyeMaterialContract(
    const nlohmann::json& materialInstanceJson,
    nlohmann::json& effectiveMaterialInstanceJson,
    const EyeResourceSet* eyeResources,
    const SubsurfaceResourceSet& subsurfaceResources,
    std::string_view materialInstancePath);

void ReapplyResolvedEyeMaterialIds(
    const ResolvedEyeMaterialAssets& resolvedAssets,
    ::MaterialInstance& materialInstance);

} // namespace VL