#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Eigen/Dense>

#include "particles/particleAsset.h"
#include "particles/particleDeterminism.h"

namespace VL
{

struct RenderScene;

// renderMode=="light" 的 CPU 运行时。它不拥有 GPU 粒子池，也不写
// sceneColor；每次 Update 把存活粒子映射成场景 PointLight，由现有的
// Light SSBO / deferred lighting 路径消费。
//
// 身份与 ParticleRenderer 保持一致：场景对象名 + 资产路径 + 发射器名。
// World 换代或发射器集合变化会重置对应状态；固定的随机种子和顺序化
// 更新保证同一输入产生同一组灯光。
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
        return static_cast<uint32_t>(emitterStates.size());
    }

private:
    struct LiveLight
    {
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        Eigen::Vector3f velocity = Eigen::Vector3f::Zero();
        float age = 0.0f;
        float lifetime = 1.0f;
        uint64_t lightId = 0;
    };

    struct EmitterState
    {
        std::string key;
        ParticleEmitterAsset emitter;
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        Eigen::Vector4f colorTint = Eigen::Vector4f::Ones();
        float spawnRateScale = 1.0f;
        uint32_t order = 0;
        uint32_t restartEpoch = 0;
        uint64_t signature = 0;
        uint64_t randomState = 0x9E3779B97F4A7C15ull;
        ParticleFixedStepState fixedStep;
        // burst 重触发时递增；即使重触发后序号发生回绕，动态灯 ID 也不会
        // 与上一批灯光相同，避免 RenderScene 中残留同 ID 快照。
        uint64_t burstGeneration = 0;
        std::vector<LiveLight> lights;
        float spawnAccumulator = 0.0f;
        float burstTimer = 0.0f;
        uint64_t nextSerial = 0;
    };

    void ResetEmitterState(EmitterState& state);
    void SimulateEmitterStep(
        EmitterState& state,
        float stepSeconds,
        uint32_t& globalLiveCount);
    void SpawnLight(
        EmitterState& state,
        uint32_t& globalLiveCount);
    void EmitLightSnapshot(
        const EmitterState& state,
        const LiveLight& light,
        RenderScene& renderScene);

    std::unordered_map<std::string, EmitterState> emitterStates;
    // 上一帧追加到 RenderScene::lights 的动态灯光；Update 先移除这些
    // 条目再重建，避免未消费新快照时逐帧重复堆积。
    std::unordered_set<uint64_t> dynamicLightIds;
    uint64_t worldGeneration = 0;
    bool hasWorldGeneration = false;
};

} // namespace VL
