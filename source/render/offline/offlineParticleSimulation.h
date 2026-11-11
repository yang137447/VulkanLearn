#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Eigen/Dense>

#include "particles/particleAsset.h"
#include "particles/particleRuntimeState.h"
#include "world/worldSnapshot.h"

namespace VL
{

// 离线粒子预览的固定步进策略。离线输出没有帧循环，必须在准备阶段把
// 系统推进到一个确定的模拟时刻；预览秒数与步数上限同时约束开销。
struct OfflineParticlePreviewPolicy
{
    float fixedDeltaSeconds = 1.0f / 60.0f;
    float previewSeconds = 0.5f;
    uint32_t maxSteps = 32;
};

inline OfflineParticlePreviewPolicy OfflineParticlePreviewDefaults() noexcept
{
    return {};
}

// 纯函数：缩略图与离线测试共用同一份步数定义。非法输入返回 0，
// 有效输入至少推进一次且不超过 maxSteps。
inline uint32_t ComputeOfflineParticleStepCount(
    const OfflineParticlePreviewPolicy& policy) noexcept
{
    if (!std::isfinite(policy.fixedDeltaSeconds) ||
        policy.fixedDeltaSeconds <= 0.0f ||
        !std::isfinite(policy.previewSeconds) ||
        policy.previewSeconds <= 0.0f ||
        policy.maxSteps == 0)
    {
        return 0;
    }
    const double steps =
        std::ceil(static_cast<double>(policy.previewSeconds) /
                  static_cast<double>(policy.fixedDeltaSeconds));
    if (!(steps > 0.0))
    {
        return 1;
    }
    return static_cast<uint32_t>(
        std::min(steps, static_cast<double>(policy.maxSteps)));
}

// 稳定种子：离线模拟不能依赖 unordered_map 迭代顺序或进程地址。
inline uint32_t StableOfflineParticleSeed(std::string_view text) noexcept
{
    uint32_t hash = 2166136261u;
    for (const char character : text)
    {
        hash ^= static_cast<uint32_t>(
            static_cast<unsigned char>(character));
        hash *= 16777619u;
    }
    return hash;
}

// WorldSnapshot 的粒子系统来自 unordered_map；这里按名字排序，保证
// 同一 World 在任意进程中都得到同一条合成顺序。
struct OfflineParticleSystemInput
{
    std::string name;
    std::string assetPath;
    ParticleSystemAsset asset;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    bool visible = true;
    ParticleSystemRuntimeOverrides runtime;
    uint32_t seed = 0;
};

std::vector<OfflineParticleSystemInput> BuildOfflineParticleInputs(
    const WorldSnapshot& snapshot,
    uint32_t seed = 0);

// 一个粒子的 CPU 镜像；字段与 shader/particleCommon.glsl 的 Particle
// 完全一致，便于逐项对照模拟实现。
struct OfflineParticle
{
    Eigen::Vector4f positionAge = Eigen::Vector4f::Zero();
    Eigen::Vector4f velocityLife = Eigen::Vector4f::Zero();
    std::array<Eigen::Vector4f, 16> history{};

    bool Alive() const noexcept
    {
        return velocityLife.w() > 0.0f &&
            positionAge.w() < velocityLife.w();
    }
};

struct OfflineParticleEmitterState
{
    ParticleEmitterAsset asset;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Vector3f worldCenter = Eigen::Vector3f::Zero();
    float worldRadius = 0.0f;
    bool visible = true;
    bool enabled = true;
    float spawnRateScale = 1.0f;
    Eigen::Vector4f colorTint = Eigen::Vector4f::Ones();
    // 模拟时间与 runtime 的 emitter.elapsedSeconds 同义：既是 burst 周期
    // 判据，也是 flipbook/噪声相位与每帧随机种子的输入。
    float elapsedSeconds = 0.0f;
    bool prewarm = true;
    std::vector<OfflineParticle> particles;
};

struct OfflineParticleSystemState
{
    std::string name;
    std::string assetPath;
    // 离线专用种子。混入每帧随机状态；seed=0 时与 GPU shader 的随机流
    // 完全一致，非零值只用于离线测试/可重复扰动。
    uint32_t seed = 0;
    bool visible = true;
    std::vector<OfflineParticleEmitterState> emitters;
};

OfflineParticleSystemState CreateOfflineParticleSystemState(
    const OfflineParticleSystemInput& input);

// 推进一个固定步长。隐藏/禁用以及运行时模板掩码关闭的发射器保持冻结，
// 与 GPU 路径不推进 elapsed/prewarm 的行为一致。
void StepOfflineParticleSystem(
    OfflineParticleSystemState& system,
    float deltaSeconds,
    const Eigen::Vector3f& cameraPosition);

OfflineParticleSystemState SimulateOfflineParticles(
    const OfflineParticleSystemInput& input,
    const OfflineParticlePreviewPolicy& policy,
    const Eigen::Vector3f& cameraPosition);

} // namespace VL
