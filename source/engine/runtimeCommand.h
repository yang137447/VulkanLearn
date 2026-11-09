#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <vector>

#include "baseStructs.h"
#include "ui/uiAction.h"
#include "debug/debugDrawControl.h"
#include "particles/particleDataChannel.h"

namespace VL
{

enum class RuntimeCommandType
{
    LoadWorld,
    // 同一粒子预览 World 内替换作者资产；不重新执行场景加载事务。
    UpdateParticlePreview,
    RunShaderReloadTest,
    RunShaderComputeReloadTest,
    RunWorldGraphTransactionTest,
    // stringValue=相对资源根的文件名，intValue=每个 emitter 的最大明细粒子数。
    DumpParticles,
    // 打印上一帧粒子发射器/绘制/排序预算；不改变渲染状态。
    ReportParticleStats,
    // 运行时粒子实例管理：stringValue=场景对象名。
    // Spawn 另用 secondaryStringValue=PS_*.json 路径、particleSpawnPosition=位置；
    // Destroy/Restart 只按名字作用于当前 World 实例。
    SpawnParticleSystem,
    DestroyParticleSystem,
    RestartParticleSystem,
    // 逐实例运行时参数覆盖：stringValue=场景对象名，
    // secondaryStringValue=spawnRateScale/colorTint/emitter/reset。
    // 覆盖只存在于当前 World，不写 PS_*.json / SC_*.json。
    SetParticleSystemParameter,
    // 发射器级运行时生命周期；索引使用 stable emitter index，name 在
    // 不存在稳定索引时作为回退身份。源索引只用于 spawn 的模板。
    SpawnParticleEmitter,
    DestroyParticleEmitter,
    RestartParticleEmitter,
    // 粒子级运行时命令：stringValue=场景对象名，particleEmitterName=发射器名。
    // KillParticle 使用 intValue=稳定槽位索引；SpawnParticleAt 使用
    // particleSpawnPosition 与 floatValue=寿命（0 取 emitter.lifetimeMax）。
    KillParticle,
    SpawnParticleAt,
    // 实例/World 粒子预算与命名用户参数；都是 runtime-only。
    SetParticleSystemBudget,
    SetParticleSystemUserParameter,
    // 附着到网格场景对象或 Blueprint 组件；local TRS 由命令携带。
    SetParticleSystemAttachment,
    // 全局粒子数据通道：name 在 particleDataChannelName；clear 由
    // particleDataChannelClear 区分。类型化值走 particleDataChannelValue，
    // 旧 floatValue 仍作为 Float 兼容路径。通道只写当前 active World。
    SetParticleDataChannel,
    // 打印 active World 的全局粒子数据通道快照。
    ReportParticleDataChannels,
    // 打印 World 侧 CPU 作者计数/预算；alive 只有渲染器回填后才可用。
    ReportParticleTelemetry,
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
    // SetParticleSystemParameter 的第二个整数参数（emitter 的 0/1 开关），
    // 与 intValue 分开以免把 index 和布尔值编码进同一个数。
    int intValue2 = 0;
    float floatValue = 0.0f;
    // 环境压力测试通过值语义投递完整天空参数，执行端只在 active World 所有权侧写回。
    SkyParametersGPU skyParametersValue;
    // 相机命令通过值语义跨越 Console -> GT，避免 Console 线程持有 World/Camera 裸指针。
    Eigen::Vector3f cameraPositionValue = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraLookAtValue = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraUpValue = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    std::string stringValue;
    // particlespawn 的资产路径；与 stringValue 分开，避免两种语义挤进一个字段。
    std::string secondaryStringValue;
    // particlespawn 的世界空间摆放位置。
    Eigen::Vector3f particleSpawnPosition = Eigen::Vector3f::Zero();
    // particleparam colorTint 的 RGBA；RGB 是 HDR 乘数，A 是不透明度乘数。
    Eigen::Vector4f particleColorTint = Eigen::Vector4f::Ones();
    // 发射器级命令的显示名；为空时由执行端生成 runtime.<stableIndex>。
    std::string particleEmitterName;
    // 命名用户参数 key 与附着目标。
    std::string particleUserParameterName;
    // 全局粒子数据通道的 key；与逐实例用户名参数分开，避免一个字段承载
    // 两种归属语义。
    std::string particleDataChannelName;
    bool particleDataChannelClear = false;
    ParticleDataChannelValue particleDataChannelValue;
    bool particleDataChannelTyped = false;
    std::string particleAttachmentTarget;
    std::string particleAttachmentComponent;
    Eigen::Vector3f particleAttachmentPosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f particleAttachmentRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f particleAttachmentScale = Eigen::Vector3f::Ones();
    std::string sourceText;
    EditorActorPropertyEdit actorProperty;
    Debug::Control debugDrawControl;
    // LoadWorld 的附加场景对象（JSON 对象数组）。模型编辑器用它把"当前打开的模型"
    // 并入中性预览环境，从而不必生成临时场景文件 —— 临时文件会在磁盘上留下第二份真相，
    // 并且在异常退出时残留。普通场景加载这里是空数组。
    nlohmann::json extraSceneObjects = nlohmann::json::array();
    // PreviewParticle 首次加载/UpdateParticlePreview 携带的草稿及其资源路径。
    std::string particleDocumentPath;
    nlohmann::json particleDocument = nullptr;
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
