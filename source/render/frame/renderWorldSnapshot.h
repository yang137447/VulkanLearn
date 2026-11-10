#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "render/resource/placementBindingResolver.h"

namespace VL
{

struct RenderScene;
class RendererResourceCache;

// Owns the per-placement renderable tracking used by the frame path. Ensure
// clones template renderables through the generic placement resolver; Prune
// retires instances that have been absent for two snapshots through the shared
// ResourceRetireQueue epoch window.
class PlacementRenderableRuntime
{
public:
    void EnsurePlacementRenderables(
        const VL::RenderScene& renderScene,
        VL::RendererResourceCache& resourceCache);
    void PrunePlacementRenderables(const VL::RenderScene& renderScene);

private:
    // 放置级 renderable 键 → 连续缺失帧数。只在 RT 访问；World 换代时清空
    // （整包由 transaction 退休）。
    std::unordered_map<std::string, int> absentFrames;
    // 放置级 renderable 键 → debugName；退休日志需要对象身份，不能只报键。
    std::unordered_map<std::string, std::string> objectNames;
    // 上一帧放置级 draw 数；只在变化时打一条日志，兼作 spawn/destroy 的存活计数。
    size_t liveCount = 0;
    // 活动包里 placement-scoped renderable 数；变化时打一条，作为无泄漏核对。
    size_t cachedPlacementCount = 0;
    // Ensure 创建、尚未退休的运行时放置 renderable 数；destroy 后应回到 0。
    size_t runtimeCount = 0;
    // 放置绑定解析结果的跨实例缓存，键见 MakePlacementBindingCacheKey。
    VL::PlacementBindingCache bindingCache;
    uint64_t worldGeneration = 0;
};

} // namespace VL

