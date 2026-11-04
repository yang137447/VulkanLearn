#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "particles/particleAsset.h"
#include "particles/particleCpuEmitterRuntime.h"

namespace VL
{

struct RenderScene;

// renderMode=="light" 的 CPU 运行时。它不拥有 GPU 粒子池，也不写
// sceneColor；每次 Update 把共享 CPU 模拟中的存活粒子映射成场景
// PointLight，由现有的 Light SSBO / deferred lighting 路径消费。
class ParticleLightRuntime
{
public:
    static constexpr uint32_t kMaxLiveLights = 64;

    void Reset();
    void Synchronize(
        const RenderScene& renderScene,
        const std::unordered_map<std::string, ParticleSystemAsset>&
            resolvedSystemAssets = {});
    void Update(RenderScene& renderScene);

    uint32_t GetLiveLightCount() const;
    uint32_t GetTrackedEmitterCount() const
    {
        return simulation.GetTrackedEmitterCount();
    }

private:
    void EmitLightSnapshot(
        const ParticleCpuEmitterRuntime::EmitterState& state,
        const ParticleCpuEmitterRuntime::Particle& particle,
        RenderScene& renderScene);

    ParticleCpuEmitterRuntime simulation;
    // 上一帧追加到 RenderScene::lights 的动态灯光；Update 先移除这些
    // 条目再重建，避免未消费新快照时逐帧重复堆积。
    std::unordered_set<uint64_t> dynamicLightIds;
};

} // namespace VL
