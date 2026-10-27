#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <vector>

#include "baseStructs.h"
#include "ui/uiAction.h"
#include "debug/debugDrawControl.h"
#include "hair/tressfxLiveParams.h"

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
    // 正常 Console 的 actor placement 入口（`mesh transform/offset/sweep`）。
    // 只在显式命令时改变 mesh 场景对象的根 TRS，不是每帧自动运行；执行端复用
    // World::SetMeshSceneObjectTransform，不经过编辑器 sceneDocuments。
    SetMeshPlacement,
    SetProceduralSkyParameters,
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
    // 控制 C# 蓝图的 Play/Pause/Stop：intValue = ScriptPlayState
    // （0 Editing / 1 Playing / 2 Paused）。与编辑器 Play 按钮走同一个
    // ScriptRuntime::Control，供无 GUI 烟测/自动化驱动；不是 runtime test flag。
    SetScriptPlayState,
    InjectInput,
    ReportShaderCacheStatistics,
    // 打印最近一次读回的逐 pass GPU 计时表；未开启采集时同时启动采集。
    PrintGpuPassStats,
    // 打印与 Performance 页同源的单行 JSON；未开启采集时同时启动采集。
    PrintPerformanceJson,
    // 正常 Console `hair trace <path> <frames> <guide-stride>`：一次性 GPU
    // 只读回读，不是 runtime test flag，也不注册 CTest。
    RequestHairTrace,
    // Console `hair reset [objectName]` / 毛发视口复位按钮：stringValue 是目标
    // 渲染对象名（空 = 所有 GPU guide 对象）。GT 只登记值语义，渲染线程在
    // 下一个正 delta 帧对该对象走一次 ResetPositions；不用重载 World 实现。
    ResetHairSimulation,
    // `hair set <objectName> <field> <value...>`：只改目标渲染对象的模拟参数副本，
    // 下一帧组 params UBO 时生效；不重载 World、不改资产文件、不改共享导入结果。
    SetHairSimulationParameter
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

// placement 命令的三种语义：绝对 TRS、在当前根位置上叠加位移、有限时长线性推进。
enum class MeshPlacementMode
{
    Absolute,
    Offset,
    Sweep
};

// Console `mesh` 命令的参数包。identity 是场景对象身份（不是 section 键）；sweep 用
// 第二个身份同时驱动角色与其发丝。position 是厘米，rotation 是度（与场景 JSON 的
// rotation 语义一致，Y 为 yaw），scale 一律为 1。
struct MeshPlacementRequest
{
    std::string identity;
    std::string secondIdentity;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f delta = Eigen::Vector3f::Zero();
    float seconds = 0.0f;
};

struct RuntimeCommand
{
    RuntimeCommandType type = RuntimeCommandType::SetDebugViewMode;
    BloomParameter bloomParameter = BloomParameter::Strength;
    RuntimeShaderReloadScope shaderReloadScope =
        RuntimeShaderReloadScope::Changed;
    int intValue = 0;
    float floatValue = 0.0f;
    // `hair trace` 的帧数与 guide 步长；path 复用 stringValue。
    uint32_t hairTraceFrames = 0;
    uint32_t hairTraceGuideStride = 0;
    // 环境压力测试通过值语义投递完整天空参数，执行端只在 active World 所有权侧写回。
    SkyParametersGPU skyParametersValue;
    // 相机命令通过值语义跨越 Console -> GT，避免 Console 线程持有 World/Camera 裸指针。
    Eigen::Vector3f cameraPositionValue = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraLookAtValue = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraUpValue = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    std::string stringValue;
    std::string sourceText;
    EditorActorPropertyEdit actorProperty;
    MeshPlacementMode meshPlacementMode = MeshPlacementMode::Absolute;
    MeshPlacementRequest meshPlacement;
    // `hair set` 的值语义：对象名 + 字段 + 1..3 个 float；GT 校验后转渲染线程。
    VL::Hair::HairLiveParameterRequest hairLiveParameter;
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
