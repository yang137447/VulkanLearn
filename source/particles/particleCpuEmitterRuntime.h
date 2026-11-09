#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Eigen/Dense>

#include "particles/particleAsset.h"
#include "particles/particleDeterminism.h"

namespace VL
{

struct RenderScene;

// CPU 侧发射器（当前只剩 renderMode=="decal"）使用的粒子模拟。它不拥有
// GPU 池，也不进入 compute simulate、indirect draw 或排序；这里只维护
// 确定性的存活粒子列表，由上层映射成 DecalSnapshot。renderMode=="light"
// 已改走 GPU 模拟池与 particleLightEmit compute，不再使用本类。
//
// 身份与 ParticleRenderer 保持一致：场景对象名 + 资产路径 + 发射器名。
// fixedTimeStep、burst 绝对周期、shape/gravity/drag 与 SplitMix64 随机都只
// 实现这一份，避免 light/decal 两条路径随时间漂移。
class ParticleCpuEmitterRuntime
{
public:
    struct Particle
    {
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        Eigen::Vector3f velocity = Eigen::Vector3f::Zero();
        float age = 0.0f;
        float lifetime = 1.0f;
        // 发射器内稳定槽位，decal 用它生成可复用纹理集的 decalId。
        uint32_t slot = 0;
        // 发射顺序号，light 保持原有的动态 lightId 语义。
        uint64_t serial = 0;
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
        // burst 重触发时递增；light 的动态 ID 继续把该代际包含在身份里。
        uint64_t burstGeneration = 0;
        std::vector<Particle> particles;
        float spawnAccumulator = 0.0f;
        // 粒子年龄/位移实际推进的模拟时间。
        float elapsedSeconds = 0.0f;
        // burst 独立时钟。非固定步长帧使用未裁剪的 raw delta，让单帧
        // 大跳也能按绝对周期只重触发一次；粒子年龄/位置仍受 0.1 s
        // 稳定性上限保护。
        float burstElapsedSeconds = 0.0f;
        bool initialBurstDone = false;
        uint64_t nextSerial = 0;
    };

    void Reset();
    void Synchronize(
        const RenderScene& renderScene,
        const std::unordered_map<std::string, ParticleSystemAsset>&
            resolvedSystemAssets,
        std::string_view renderMode,
        // 已经由 GPU 池接管的 emitter key；这些 key 不建立/保留 CPU 状态，
        // 使 GPU 失败时下一次 Synchronize 能自然回落到 CPU 模拟。
        const std::unordered_set<std::string>* excludeKeys = nullptr);
    // capacity 是本帧允许存在的 CPU 粒子总数。超出时按 particleSystems /
    // emitter 顺序保留前面的粒子，并从后往前截断。
    void Simulate(const RenderScene& renderScene, uint32_t capacity);

    const std::vector<EmitterState*>& GetOrderedStates() const noexcept
    {
        return orderedStates;
    }

    uint32_t GetLiveParticleCount() const;
    uint32_t GetTrackedEmitterCount() const
    {
        return static_cast<uint32_t>(emitterStates.size());
    }

private:
    void ResetEmitterState(EmitterState& state);
    void AdvanceEmitterStep(
        EmitterState& state,
        float stepSeconds,
        uint32_t& globalLiveCount);
    void SpawnParticle(
        EmitterState& state,
        uint32_t capacity,
        uint32_t& globalLiveCount);
    void TrimToCapacity(uint32_t capacity, uint32_t& globalLiveCount);

    std::unordered_map<std::string, EmitterState> emitterStates;
    std::vector<EmitterState*> orderedStates;
    uint64_t worldGeneration = 0;
    bool hasWorldGeneration = false;
};

} // namespace VL
