#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <vector>

#include "baseStructs.h"
#include "ui/uiAction.h"
#include "debug/debugDrawControl.h"
#include "particles/particleDataChannel.h"
#include "hair/tressfxLiveParams.h"
#include "hair/tressfxGroupPaint.h"
#include "world/actorProperty.h"

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
    ImpactSoftBody,
    ImpactGlassPane,
    FireProjectile,
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
    ReportTerrainFeedbackStatistics,
    ReportTerrainStatistics,
    ReportTerrainLayerStatistics,
    ReportTerrainLodStatistics,
    // 几何 chunk 驻留/显存基线：加载期创建的级与 chunk 数、顶点/索引字节、
    // 当前帧实际出图数。只读诊断，不改渲染状态。
    ReportTerrainChunkStatistics,
    ReportTerrainCollisionStatistics,
    // Console `terrain parity [tileX tileZ]`：同一覆盖页 CPU/GPU 烘焙逐 texel
    // 比较；intValue = tileX * 32 + tileZ，渲染线程帧边界同步执行一次。
    RunTerrainPageBakeParity,
    SetTerrainResidentCapacity,
    // L4a：层页预算与对照全量预取；都只排队到渲染线程帧首执行。
    SetTerrainLayerResidentCapacity,
    RequestTerrainLayerPreload,
    // L4e：强制目标 mip（-1 = 自动），只影响 mode 1，用于复现缺页回退。
    SetTerrainLayerForceMip,
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
    SetHairSimulationParameter,
    // 头发分组预览：只写 active World 的会话状态；渲染线程从冻结快照消费，
    // 不直接改 RenderableObject 或 GPU buffer。
    SetTressfxGroupPreview,
    // 草稿 group binding：值语义进入 RT 队列，在 BeginFrame 后原地更新
    // RenderableObject 的 group id/颜色/参数表，不写资产。
    SetTressfxGroupBindingPreview,
    // 分组笔刷 ghost：值语义命令，渲染线程按对象名更新 host-visible preview buffer。
    SetTressfxBrushPreview,
    // Console `quit`：只在 GT 命令点置位退出意图，由 EngineLoop 走既有 Shutdown 路径。
    QuitApplication
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
    // SetParticleSystemParameter 的第二个整数参数（emitter 的 0/1 开关），
    // 与 intValue 分开以免把 index 和布尔值编码进同一个数。
    int intValue2 = 0;
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
    MeshPlacementMode meshPlacementMode = MeshPlacementMode::Absolute;
    MeshPlacementRequest meshPlacement;
    // `hair set` 的值语义：对象名 + 字段 + 1..3 个 float；GT 校验后转渲染线程。
    VL::Hair::HairLiveParameterRequest hairLiveParameter;
    // `SetTressfxGroupPreview` 的值语义；sourceText=="ui" 时必须校验 World 代数。
    int32_t tressfxSelectedGroup = -1;
    uint32_t tressfxGroupFlags = 0;
    VL::Hair::TressfxGroupBindingPreviewRequest tressfxGroupBindingPreview;
    VL::Hair::TressfxBrushPreviewRequest tressfxBrushPreview;
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
