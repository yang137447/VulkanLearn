#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "scene/sceneAssetTypes.h"
#include "world/worldBuildPlan.h"

class PipelineFactory;

namespace VL
{
class RendererBackendVulkan;
struct RendererResourceLoadContext;

// 依据 World 已注册的虚拟页数推导驻留容量：下限是 defaultCapacity
// （2km/32×32 宽视野 working set 的实测预算），上限是物理槽位数。
// 容量若小于注册页数，LRU 会在世界内部切出一条直线：一侧常驻、另一侧
// 永远缺页。纯函数，单测直接锁定。
inline uint32_t ComputeTerrainResidentCapacity(
    uint32_t registeredPages,
    uint32_t defaultCapacity,
    uint32_t usableSlots)
{
    return std::min(std::max(defaultCapacity, registeredPages), usableSlots);
}

// 渲染器只返回网格/风计划与 gameplay 材质解析结果；gameplay 参数与对象
// 构造属于 WorldLoader / WorldBuilder。
using RendererWorldResourceLoadResult = WorldAssemblyAssets;

// Coordinates renderer resource loading for one validated WorldBuildPlan.
// WorldLoader/WorldBuilder own scene IO, validation, gameplay plans, camera,
// lights, and World identity; concrete mesh/material/environment creation stays
// in the renderer loaders and is registered in RendererResourceCache.
class RendererResourceLoadCoordinator
{
public:
    static RendererResourceLoadCoordinator& GetInstance()
    {
        static RendererResourceLoadCoordinator instance;
        return instance;
    }

    void SetPipelineFactory(PipelineFactory* pipelineFactory);
    void SetRendererBackend(RendererBackendVulkan* rendererBackend);
    RendererWorldResourceLoadResult LoadRendererResources(
        const WorldBuildPlan& worldBuildPlan,
        RendererResourceLoadContext& loadContext);

private:
    RendererResourceLoadCoordinator();

    PipelineFactory* pipelineFactory = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
};

} // namespace VL
