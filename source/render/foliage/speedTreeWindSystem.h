#pragma once

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../../BaseStructs.h"
#include "../../mesh/meshAssetTypes.h"
#include "../../world/worldWind.h"

// Renderer-owned live state for one active Runtime SDK wind profile. The
// authored profile remains on ModelResource; this class owns the per-species
// CWindStateMgr-equivalent response/gust/noise state and produces the compact
// per-frame shader state.
//
// 运行时路径是 AdvanceToSceneWind：场景 wind 只广播作者目标（方向/基准强度/
// gust 开关与幅度）；本类按该物种的 strengthResponse/directionResponse 缓动，
// 按 profile 的 gustFrequency/强度与持续时间/rise-fall 推进离散 gust。所有实例
// 共享同一 base tree 状态，实例差异由世界位置和顶点权重提供。
//
// SetStrength / SetDirection / SetGustingEnabled / ForceGust / AdvanceTo 仍是
// SDK 自驱动参考状态机，tool/speedtree-wind-validation 直接对照；AdvanceToSceneWind
// 复用同一套状态推进，但目标值来自场景风。
class SpeedTreeWindSystem
{
public:
    SpeedTreeWindSystem() = default;

    void Reset();
    void Configure(
        const WindResponse& config,
        const Eigen::Vector3f& sourceBoundsMin,
        const Eigen::Vector3f& sourceBoundsMax);
    void SetStrength(float strength);
    void SetDirection(const Eigen::Vector3f& direction);
    void SetGustingEnabled(bool enabled);
    void ForceGust();
    void AdvanceTo(double timeSeconds);
    // timeSeconds 为固定 tick 的风时间（秒）；wind 是场景作者目标。本类按该
    // profile 的响应时间缓动，并使用 profile 的 gust 参数；场景 gust.amplitude
    // 只作为整体阵风强度缩放，frequency/turbulence 留给场级消费者（头发）。
    void AdvanceToSceneWind(double timeSeconds, const VL::WorldWind& wind);

    const SpeedTreeWindStateGPU& GetGpuState() const { return gpuState; }
    bool IsConfigured() const { return configured; }
    bool IsGustingEnabled() const { return gustingEnabled; }

private:
    struct GustState
    {
        float strength = 0.0f;
        float targetStrength = 0.0f;
        float strengthAtStart = 1.0f;
        double startTime = 0.0;
        double riseTargetTime = 0.0;
        double fallStartTime = 0.0;
        double fallTargetTime = 0.0;
        bool forceRequested = false;
    };

    WindResponse config;
    SpeedTreeWindStateGPU gpuState;
    Eigen::Vector3f sourceBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceBoundsMax = Eigen::Vector3f::Zero();
    Eigen::Vector3f treeExtents = Eigen::Vector3f::Ones();
    Eigen::Vector3f windDirection = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Vector3f currentWindDirection = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Vector3f windDirectionAtStart = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Quaternionf windDirectionRotation = Eigen::Quaternionf::Identity();
    float targetStrength = 0.35f;
    float currentStrength = 0.35f;
    float strengthAtStart = 0.35f;
    double strengthChangeStartTime = 0.0;
    double strengthChangeEndTime = 0.0;
    double directionChangeStartTime = 0.0;
    double directionChangeEndTime = 0.0;
    GustState gustState;
    uint32_t gustRandomState = 137u;
    bool gustingEnabled = true;
    // 场景 gust.amplitude 对 profile gust 输出的整体缩放；SDK 对照路径保持 1。
    float sceneGustScale = 1.0f;
    Eigen::Vector3f sharedNoisePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f branch1NoisePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f branch2NoisePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f rippleNoisePosition = Eigen::Vector3f::Zero();
    double lastAdvanceTime = -1.0;
    bool configured = false;

    void SetSceneGustScale(float scale);
    void ResetGustState();
    float RandomFloat(float minimum, float maximum);
    // 把强度向 targetStrength 缓动到 time 时刻的值；SDK 参考状态机和场景
    // 目标路径共用。
    void UpdateEasedStrength(double time);
    // 由风向与归一化强度生成 GPU 状态，并按 deltaTime 推进四组噪声位置。
    // 两条驱动路径共用，保证响应曲线采样与 UBO 打包只有一份实现。
    void UpdateGpuState(const Eigen::Vector3f& direction, float strength, float deltaTime);
};

// Owns one live CPU state per imported tree species. The runtime wind input is
// the authored scene target; each profile then advances its own response, gust
// envelope and noise state like a SpeedTree CWindStateMgr. Draw submission
// resolves the state by the mesh profile key and uploads it through the object
// UBO. The SetStrength/SetDirection/Gust methods below are the SDK reference
// controls and are not called by the renderer.
class WindResponseProfileSet
{
public:
    void Configure(const std::unordered_map<std::string, WindResponseProfile>& profiles);
    void Reset();
    void Swap(WindResponseProfileSet& other) noexcept;
    void AdvanceTo(double timeSeconds);
    void AdvanceToSceneWind(double timeSeconds, const VL::WorldWind& wind);
    // Expects a finite normalized strength in [0, 1]. External input must be
    // validated at its source before reaching the wind simulation.
    bool SetStrength(float strength);
    bool SetDirection(const Eigen::Vector3f& direction);
    bool SetGustingEnabled(bool enabled);
    bool ForceGust();

    const SpeedTreeWindStateGPU* FindGpuState(std::string_view profileKey) const;
    size_t GetProfileCount() const { return systems.size(); }
    bool IsGustingEnabled() const;

private:
    std::unordered_map<std::string, SpeedTreeWindSystem> systems;
};
