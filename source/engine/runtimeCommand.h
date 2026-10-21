#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <vector>

#include "baseStructs.h"
#include "ui/uiAction.h"
#include "debug/debugDrawControl.h"

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
    ControlAnimation,
    // 被聚焦脚本控制器：stringValue=场景网格身份，intValue 0=seek/1=play，floatValue=秒。
    ControlScriptAnimation,
    ControlMotionDebug,
    ControlMotionReplay,
    ControlDebugDraw,
    SetEnvironmentIntensity,
    SetSunIntensity,
    // 逐 actor 可见性：stringValue 是大纲的稳定身份（"mesh|Sphere_01"、
    // "directionalLight|Sun_Light"），intValue 是 0/1。身份在执行端解析并核对，
    // 未知/陈旧身份必须被拒绝并给诊断。可见性是 World 的运行时状态，不持久化。
    SetActorVisible,
    SetActorProperty,
    SetProceduralSkyParameters,
    SetSpeedTreeStrength,
    SetSpeedTreeGustingEnabled,
    ForceSpeedTreeGust,
    SetToneMappingMode,
    SetCameraExposure,
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
    ReloadAssembly,
    InjectInput,
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
    EditorActorPropertyEdit actorProperty;
    Debug::Control debugDrawControl;
    // LoadWorld 的附加场景对象（JSON 对象数组）。模型编辑器用它把"当前打开的模型"
    // 并入中性预览环境，从而不必生成临时场景文件 —— 临时文件会在磁盘上留下第二份真相，
    // 并且在异常退出时残留。普通场景加载这里是空数组。
    nlohmann::json extraSceneObjects = nlohmann::json::array();
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
