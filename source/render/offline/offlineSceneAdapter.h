#pragma once

#include <nlohmann/json.hpp>

#include "core/renderSession.h"
#include "core/runtimeResult.h"
#include "render/offline/offlineWorldAssets.h"
#include "world/worldSnapshot.h"

namespace VL
{

struct OfflineRenderSettings
{
    int width = 0;
    int height = 0;
    int samples = 0;
    int bounces = 0;
    int lightSamples = 1;
    std::uint32_t seed = 0;
    bool interactivePreview = false;
};

Offline::FabricMaterial ParseOfflineClothProfile(const nlohmann::json& profile);

// 只读冻结包与帧快照；不访问 live World/MI 或 Vulkan 资源。失败携带对象及
// 资产路径，调用者不能把不支持项替换为默认材质继续计算。
RuntimeResult<Offline::RenderInput> BuildOfflineRenderInput(
    const WorldSnapshot& snapshot,
    const OfflineWorldAssets& assets,
    const OfflineRenderSettings& settings,
    const std::atomic<std::uint64_t>* latestGeneration = nullptr,
    std::uint64_t expectedGeneration = 0);

// 事务/交互边界只检查能力和输入；三角形转换及 BVH 留给 CPU prepare worker。
RuntimeResult<bool> PreflightOfflineRenderInput(
    const WorldSnapshot& snapshot,
    const OfflineWorldAssets& assets,
    const OfflineRenderSettings& settings);

} // namespace VL
