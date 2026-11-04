#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "particles/particleAsset.h"
#include "particles/particleCpuEmitterRuntime.h"
#include "render/decal/decalLimits.h"

namespace VL
{

struct RenderScene;

// renderMode=="decal" 的 CPU 运行时。共享 light 的固定步长/burst/shape/
// gravity/drag 模拟，但不分配 GPU 粒子池；每个存活粒子映射成一个
// BeforeLighting DBuffer DecalSnapshot。
class ParticleDecalRuntime
{
public:
    void Reset();
    void Synchronize(
        const RenderScene& renderScene,
        const std::unordered_map<std::string, ParticleSystemAsset>&
            resolvedSystemAssets = {});
    void Update(
        RenderScene& renderScene,
        uint32_t maxTotalDecals = kMaxDecalInstances);

    uint32_t GetLiveDecalCount() const
    {
        return simulation.GetLiveParticleCount();
    }
    uint32_t GetTrackedEmitterCount() const
    {
        return simulation.GetTrackedEmitterCount();
    }

private:
    void RemoveDynamicDecals(RenderScene& renderScene);
    void EmitDecalSnapshot(
        const ParticleCpuEmitterRuntime::EmitterState& state,
        const ParticleCpuEmitterRuntime::Particle& particle,
        RenderScene& renderScene);

    ParticleCpuEmitterRuntime simulation;
    // 上一帧追加到 RenderScene::decals 的动态条目；每帧先删再建，避免
    // 静态 decal 与粒子 decal 混在一起逐帧堆积。
    std::unordered_set<uint64_t> dynamicDecalIds;
};

} // namespace VL
