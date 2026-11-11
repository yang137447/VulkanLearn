#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Eigen/Dense>

#include "particles/particleAsset.h"
#include "particles/particleCpuEmitterRuntime.h"
#include "particles/particleDecalGpu.h"
#include "render/decal/decalLimits.h"

namespace VL
{

struct RenderScene;

// renderMode=="decal" 的运行时。GPU 池可用时只消费 particleDecalGather.comp
// 回读的紧凑记录；没有 GPU 池的 emitter（或 GPU 路径首次尚未产出样本）
// 继续保留原有 CPU 模拟作为 fallback。两条路径共享 64 槽预算与稳定 decalId。
class ParticleDecalRuntime
{
public:
    void Reset();
    void Synchronize(
        const RenderScene& renderScene,
        const std::unordered_map<std::string, ParticleSystemAsset>&
            resolvedSystemAssets = {},
        const std::unordered_map<
            std::string, std::vector<ParticleDecalGpuInstance>>&
            gpuInstances = {});
    void Update(
        RenderScene& renderScene,
        uint32_t maxTotalDecals = kMaxDecalInstances,
        const std::unordered_map<
            std::string, std::vector<ParticleDecalGpuInstance>>&
            gpuInstances = {});

    // 本帧真正写入 RenderScene::decals 的动态条目数（GPU 回读 + CPU fallback）。
    uint32_t GetLiveDecalCount() const
    {
        return liveDecalCount;
    }
    // 仅统计仍在 CPU fallback 路径上的 decal emitter。
    uint32_t GetTrackedEmitterCount() const
    {
        return simulation.GetTrackedEmitterCount();
    }

private:
    struct GpuEmitterState
    {
        std::string key;
        ParticleEmitterAsset emitter;
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        Eigen::Vector4f colorTint = Eigen::Vector4f::Ones();
    };
    struct OrderedEmitter
    {
        std::string key;
        bool gpu = false;
    };

    void RemoveDynamicDecals(RenderScene& renderScene);
    void EmitDecalSnapshot(
        const ParticleCpuEmitterRuntime::EmitterState& state,
        const ParticleCpuEmitterRuntime::Particle& particle,
        RenderScene& renderScene);
    void EmitDecalSnapshotFromGpu(
        const GpuEmitterState& state,
        const ParticleDecalGpuInstance& instance,
        RenderScene& renderScene);

    ParticleCpuEmitterRuntime simulation;
    // 上一帧追加到 RenderScene::decals 的动态条目；每帧先删再建，避免
    // 静态 decal 与粒子 decal 混在一起逐帧堆积。
    std::unordered_set<uint64_t> dynamicDecalIds;
    // 场景顺序的 decal emitter 列表；GPU/CPU 同序，预算截断保持确定性。
    std::vector<OrderedEmitter> orderedEmitters;
    std::unordered_map<std::string, GpuEmitterState> gpuEmitters;
    uint32_t liveDecalCount = 0;
};

} // namespace VL
