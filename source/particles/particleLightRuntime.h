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

// CPU 参考实现，仅保留给模块测试/行为对照。实时 RenderSystem 不再调用它：
// renderMode=="light" 由 GPU 模拟池 + particleLightEmit compute 生成
// ParticleLightSSBO，因此不会与场景 PointLight 重复发光。
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
