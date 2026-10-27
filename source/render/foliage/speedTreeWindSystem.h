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
// authored profile remains on ModelResource; this class only samples curves
// and produces the compact per-frame shader state.
//
// 运行时路径是 AdvanceToSceneWind：风向与强度全部来自场景 WorldWind，物种只提供
// 响应曲线的数值。SetStrength / SetDirection / SetGustingEnabled / ForceGust /
// AdvanceTo 保留的是 SDK 自驱动参考状态机，仅供 tool/speedtree-wind-validation
// 对照 SDK 使用；渲染路径不再调用它们。
class SpeedTreeWindSystem
{
public:
    SpeedTreeWindSystem() = default;

    void Reset();
    void Configure(
        const SpeedTreeWindConfig& config,
        const Eigen::Vector3f& sourceBoundsMin,
        const Eigen::Vector3f& sourceBoundsMax);
    void SetStrength(float strength);
    void SetDirection(const Eigen::Vector3f& direction);
    void SetGustingEnabled(bool enabled);
    void ForceGust();
    void AdvanceTo(double timeSeconds);
    // timeSeconds 为固定 tick 的风时间（秒）；wind 为当前活动 World 的场景风。
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

    SpeedTreeWindConfig config;
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
    Eigen::Vector3f sharedNoisePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f branch1NoisePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f branch2NoisePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f rippleNoisePosition = Eigen::Vector3f::Zero();
    double lastAdvanceTime = -1.0;
    bool configured = false;

    void ResetGustState();
    float RandomFloat(float minimum, float maximum);
    // 把强度向 targetStrength 缓动到 time 时刻的值，SpeedTree 与场景风路径共用。
    void UpdateEasedStrength(double time);
    // 由风向与归一化强度生成 GPU 状态，并按 deltaTime 推进四组噪声位置。
    // 两条驱动路径共用，保证响应曲线采样与 UBO 打包只有一份实现。
    void UpdateGpuState(const Eigen::Vector3f& direction, float strength, float deltaTime);
};

// Owns one live CPU state per imported tree species. The runtime wind input is
// the scene WorldWind (AdvanceToSceneWind), shared by every species; draw
// submission resolves the state by the mesh profile key and uploads it through
// the object UBO. The SetStrength/SetDirection/Gust methods below are the SDK
// reference controls and are not called by the renderer.
class SpeedTreeWindProfileSet
{
public:
    void Configure(const std::unordered_map<std::string, SpeedTreeWindProfile>& profiles);
    void Reset();
    void Swap(SpeedTreeWindProfileSet& other) noexcept;
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
