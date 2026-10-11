#pragma once

#include <string>
#include <vector>

#include "baseStructs.h"
#include "ui/uiAction.h"

namespace VL
{

enum class RuntimeCommandType
{
    LoadWorld,
    RunShaderReloadTest,
    RunShaderComputeReloadTest,
    RunWorldGraphTransactionTest,
    SetDebugViewMode,
    CaptureScreenshot,
    SetCameraPosition,
    SetCameraLookAt,
    SetCameraPose,
    GetCameraState,
    SetEnvironmentIntensity,
    SetSunIntensity,
    // 逐 actor 可见性：stringValue 是大纲的稳定身份（"mesh|Sphere_01"、
    // "directionalLight|Sun_Light"），intValue 是 0/1。身份在执行端解析并核对，
    // 未知/陈旧身份必须被拒绝并给诊断。可见性是 World 的运行时状态，不持久化。
    SetActorVisible,
    SetProceduralSkyParameters,
    SetSpeedTreeStrength,
    SetSpeedTreeGustingEnabled,
    ForceSpeedTreeGust,
    SetToneMappingMode,
    SetOfflineMode,
    SetOfflineSetting,
    SetOfflineAov,
    SetOfflineExposure,
    ControlOfflineSession,
    ExportOffline,
    SetBloomParameter,
    SetCsmCastShadows,
    SetCsmDynamicShadowDistance,
    SetCsmDynamicShadowCascades,
    SetCsmCascadeDistributionExponent,
    SetCsmCascadeTransitionFraction,
    SetCsmShadowDistanceFadeoutFraction,
    SetCsmShadowBias,
    SetCsmShadowSlopeBias,
    SetCsmShadowCascadeBiasDistribution,
    SaveCsmSettingsToScene,
    ReloadShaders,
    ReportShaderCacheStatistics
};

enum class RuntimeShaderReloadScope
{
    Changed,
    All
};

enum class BloomParameter
{
    Strength,
    Threshold,
    Knee,
    Clamp
};

struct RuntimeCommand
{
    RuntimeCommandType type = RuntimeCommandType::SetDebugViewMode;
    BloomParameter bloomParameter = BloomParameter::Strength;
    RuntimeShaderReloadScope shaderReloadScope =
        RuntimeShaderReloadScope::Changed;
    int intValue = 0;
    float floatValue = 0.0f;
    // 环境压力测试通过值语义投递完整天空参数，执行端只在 active World 所有权侧写回。
    SkyParametersGPU skyParametersValue;
    // 相机命令通过值语义跨越 Console -> GT，避免 Console 线程持有 World/Camera 裸指针。
    Eigen::Vector3f cameraPositionValue = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraLookAtValue = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraUpValue = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    std::string stringValue;
    std::string sourceText;
};

// Single-thread command queue for runtime console/debug requests. The console
// produces commands; EngineLoop drains them at a stable point before rendering.
class CommandBus
{
public:
    void Queue(RuntimeCommand command);
    void Queue(UiAction action);
    std::vector<RuntimeCommand> Drain();
    std::vector<UiAction> DrainUiActions();

private:
    std::vector<RuntimeCommand> pendingCommands;
    std::vector<UiAction> pendingUiActions;
};

} // namespace VL
