#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

#include <Eigen/Dense>

#include "baseStructs.h"
#include "decalResponse.h"
#include "environmentType.h"
#include "mesh/meshAssetTypes.h"
#include "render/shadow/csmSettings.h"
#include "render/volumetricFog/volumetricFogSettings.h"
#include "particles/particleAsset.h"
#include "particles/particleDataChannel.h"
#include "particles/particleRuntimeState.h"
#include "terrain/terrainCollisionTypes.h"
#include "scene/placementBinding.h"
#include "world/worldSnapshot.h"
#include "world/worldWind.h"
#include "world/projectileSettings.h"
#include "animation/motionDebug.h"
#include "animation/motionHistory.h"
#include "animation/motionRuntime.h"
#include "animation/animationSyncGroup.h"
#include "physics/bonePhysics.h"
#include "debug/drawDebug.h"
#include "hair/tressfxGroupPreviewState.h"
#include <map>

class Camera;
class DirectionalLight;
class PointLight;
class SceneNode;
class SpotLight;

namespace VL
{
namespace Animation { class SkeletalPlayback; struct SkinPose; }
namespace Hair { class TressfxRootCloud; }
namespace Hair { struct TressfxGroupBinding; }
class ScriptWorldContext;
struct ScriptTemplateExtension;

struct WorldEnvironment
{
    // 场景 JSON 里的环境对象名。渲染路径不读它，编辑器大纲用它给出与场景
    // 一致的行名；场景没有 environment 对象时保持为空，由 UI 回退到固定标签。
    std::string name;
    EnvironmentType type = EnvironmentType::ProceduralSky;
    uint32_t cubeSize = 128;
    float intensity = 1.0f;
    std::string hdrPath;
    SkyParametersGPU skyParameters;
    // environment.fog；没有该块时 FogSettings::enabled 为 false，
    // 渲染路径保持旧行为（不产生体积雾）。
    FogSettings fog;
};

// 全局粒子数据通道是与逐实例 userParameters 分离的命名空间：它只存在于
// 当前 World，供 v2 模块的 `$Name` 在运行时解析使用。命令入口、C# 包装和
// Console 共用同一组边界（key 上限是 UTF-8 字节数），避免三条路径对
// "合法 key/value"给出不同答案。
constexpr size_t kMaxParticleDataChannels = 64;
constexpr size_t kMaxParticleDataChannelNameLength = 64;
constexpr float kMaxParticleDataChannelAbsoluteValue = 1.0e6f;

inline bool ValidateParticleDataChannelName(std::string_view name,
    std::string& outError)
{
    if (name.empty() || name.size() > kMaxParticleDataChannelNameLength)
    {
        outError =
            "Particle data channel name requires 1..64 UTF-8 bytes.";
        return false;
    }
    if (name.find('\0') != std::string_view::npos)
    {
        outError =
            "Particle data channel name must not contain NUL.";
        return false;
    }
    outError.clear();
    return true;
}

inline bool ValidateParticleDataChannelValue(float value,
    std::string& outError)
{
    if (!std::isfinite(value) ||
        std::abs(value) > kMaxParticleDataChannelAbsoluteValue)
    {
        outError =
            "Particle data channel value must be finite and in [-1e6, 1e6].";
        return false;
    }
    outError.clear();
    return true;
}

inline bool ValidateParticleDataChannelValue(
    const ParticleDataChannelValue& value, std::string& outError)
{
    return value.Validate(outError);
}

class World;
// 运行时命令桥的单一落点：先完整校验，再调用 World 写入，任何失败都保证
// 无副作用。Console 使用上面的纯校验；执行端与测试共用这个函数。
bool ExecuteParticleDataChannelCommand(World& world, bool clear,
    const std::string& name, float value, std::string& outError);
bool ExecuteParticleDataChannelCommand(World& world, bool clear,
    const std::string& name, const ParticleDataChannelValue& value,
    std::string& outError);

struct WorldSquareAreaLight
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
    Eigen::Vector3f color = Eigen::Vector3f::Ones();
    float size = 1.0f;
    // 线性 emitted radiance，不与方向光 irradiance 混用。
    float intensity = 1.0f;
    // 与网格/灯具同样的运行时可见性。实时路径目前不接受 squareAreaLight，
    // 这里仍保留字段，让大纲的眼睛在四类灯上语义一致。
    bool visible = true;
};

// 场景中的静态 decal 组件。size 是作者空间的完整尺寸（cm），按 UE
// DecalSize 的语义，X 是投影深度、Y/Z 是投影平面宽高；decalToWorld
// 已把半尺寸烘进局部矩阵，渲染器直接用 [-1,1] 单位 cube。
struct WorldDecal
{
    std::string name;
    Eigen::Vector3f scenePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneScale = Eigen::Vector3f::Ones();
    Eigen::Vector3f size = Eigen::Vector3f(100.0f, 400.0f, 400.0f);
    Eigen::Matrix4f decalToWorld = Eigen::Matrix4f::Identity();
    Eigen::Vector4f color =
        Eigen::Vector4f(0.02f, 0.018f, 0.015f, 1.0f);
    float opacity = 0.85f;
    float edgeSoftness = 0.35f;
    float noiseStrength = 0.55f;
    float randomSeed = 0.0f;
    float roughness = 1.0f;
    float metallic = 0.0f;
    float ambientOcclusion = 1.0f;
    // 线性自发光颜色与强度；强度为 0 时不写 DBufferEmissive。
    Eigen::Vector3f emissive = Eigen::Vector3f::Zero();
    float emissiveStrength = 0.0f;
    // 作者侧的 DBuffer 响应位；DecalRenderer 把它随实例数据上传，
    // deferredLighting 按 DBufferMask 逐通道决定是否覆盖表面。
    uint32_t responseMask = DefaultDecalResponse();
    // decal 法线对接收表面法线的混合权重，0 表示忽略法线响应。
    float normalStrength = 0.35f;
    // 可选的 T_*.json 贴图资产引用；空字符串表示保留程序化径向 + noise
    // 的默认外观。decal 材质完全由代码维护，没有 UE Material Graph。
    std::string albedoMap;
    std::string normalMap;
    // ORM 打包约定与 glTF/UE 一致：R=roughness，G=metallic，B=ambientOcclusion。
    std::string ormMap;
    uint32_t sortOrder = 0;
    bool visible = true;
};

// 灯光的可见性直接落在灯光对象上（SceneNode::visible）。World 只按名字找到
// shared_ptr，不改持有关系：合并/分节后的网格没有 SceneNode，因此可见性字段
// 只在网格与灯光两处存在，不引入第二套"World 拥有的可见性集合"。
struct WorldMeshObject
{
    std::shared_ptr<Animation::SkeletalPlayback> animation;
    std::shared_ptr<const Animation::SkinPose> skinPose;
    size_t skinSection = 0;
    std::string debugName;
    // 渲染对象名和场景对象身份分开保存，避免多 section 展开后无法回到
    // scene JSON 的原始对象，从而不能为选中的 MI 构造导航上下文。
    std::string sceneObjectIdentity;
    // 逐对象可见性属于 World 的运行时游戏状态，不是场景 JSON 字段：
    // 它不参与 scene schema/validator，也不写回资产，重载场景一律从"全部可见"开始。
    bool visible = true;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    // 保留 scene JSON 的根 TRS，而非对导入后的 section 矩阵做无损性不成立的分解。
    // 编辑时用新旧根矩阵之差一次变换整 actor，保留 FBX 节点与镜像缩放。
    Eigen::Vector3f scenePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneScale = Eigen::Vector3f::Ones();
    Eigen::Vector3f localBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f localBoundsMax = Eigen::Vector3f::Zero();
    std::shared_ptr<const std::vector<Eigen::Vector3f>> collisionTriangles;
    std::string meshKey;
    std::string materialKey;
    std::string materialInstanceKey;
    std::string speedTreeWindProfileKey;
    bool terrainFeedbackEligible = false;
    // 地形几何 LOD 组与级号；非地形/单级地形为空组、级号 0。
    std::string lodGroupId;
    uint32_t lodLevel = 0;
    uint32_t lodLevelCount = 1;
    std::vector<float> lodDistancesCm;
    Eigen::Vector3f lodGroupBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f lodGroupBoundsMax = Eigen::Vector3f::Zero();
    // 地形 chunk 仍是普通 WorldMeshObject；独立 Terrain 身份只属于编辑器
    // 展示/身份层，不引入第二套 World 地形类型。
    bool isTerrain = false;
    std::string terrainAssetPath;
    // 同一 TR 的多处摆放必须能区分各自 chunk；为空表示非地形。
    std::string terrainPlacementId;
    std::string windResponseProfileKey;
    // TressFX 顶点工厂反推 guideIndex 用的每根 guide 点数；非头发为 0。
    uint32_t tressfxPointsPerGuide = 0;
    // 发根选择缓存只读共享；编辑器快照按 object.model 变换到世界空间。
    std::shared_ptr<const Hair::TressfxRootCloud> tressfxRootCloud;
    // 已保存资产的 guide->group 映射与组名；组编辑只替换这份只读绑定。
    std::shared_ptr<const Hair::TressfxGroupBinding> tressfxGroupBinding;
    // placement 解析后的每组模拟配置（0 = Default，k = 第 k 个命名组）。
    // 编辑器只读它显示 asset default；求解器仍从 placementBinding 消费。
    std::shared_ptr<const std::vector<TressfxSimulationConfig>> tressfxGroupConfigs;
    // 放置级（per-placement）网格：placementTemplateMeshKey 非空表示本对象在 spawn
    // 时从该模板 RenderableObject 克隆独立实例（共享导入几何、独立 GPU 状态）；
    // placementBinding 描述如何解析自身绑定（Kind + 参数），由渲染线程按 Kind 分派。
    std::string placementTemplateMeshKey;
    PlacementBinding placementBinding;
};

// 场景里声明的橡皮软体对象。它只保存作者参数和已解析的材质 key；
// RubberShell 粒子、动态网格和爆裂状态由 GT 侧运行时持有，不进入 World。
struct WorldSoftBodyObject
{
    std::string name;
    std::string profileKey;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Vector3f scenePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneScale = Eigen::Vector3f::Ones();
    int subdivisions = 2;
    int siteCount = 16;
    double radius = 20.0;
    double thickness = 0.2;
    double pressure = 10.0;
    double burstStrain = 0.35;
    double restContraction = 0.75;
    uint64_t seed = 0;
    std::string materialKey;
    std::string materialInstanceKey;
    bool visible = true;
};

// 场景里声明的运行时玻璃板。完整板网格、碎片和二次碎裂状态由 GT 侧
// GlassRuntime 持有；World 只保存作者参数与已解析的材质 key。
struct WorldGlassObject
{
    std::string name;
    std::string profileKey;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Vector3f scenePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneScale = Eigen::Vector3f::Ones();
    double width = 100.0;
    double height = 100.0;
    double thickness = 0.6;
    int siteCount = 32;
    uint64_t seed = 0;
    bool subfracture = true;
    std::string materialKey;
    std::string materialInstanceKey;
    std::string crackMaterialKey;
    std::string crackMaterialInstanceKey;
    bool visible = true;
};

// 粒子发射器是 World 级渲染对象；粒子缓冲本身归渲染器所有，
// World 只冻结作者资产、摆放 TRS、可见性与实例运行时覆盖，避免把 GPU 生命周期混进逻辑世界。
struct WorldParticleSystem
{
    // 发射器槽位只由 World 维护；stableIndex 等于槽位下标，销毁是 tombstone，
    // 不会重排或复用。运行时 asset.emitters 只保留 active 槽位的紧凑副本，
    // 因此渲染器资源仍能随 spawn/destroy 创建/退休，而 API 身份保持稳定。
    struct EmitterSlot
    {
        ParticleEmitterAsset asset;
        bool active = true;
        bool enabled = true;
        uint32_t restartEpoch = 0;
    };

    std::string name;
    std::string assetPath;
    ParticleSystemAsset asset;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    // 与网格/灯光同源的作者变换缓存：Details 回显 TRS，编辑器改轴时
    // 用同一组值重算 model，不从已烘焙矩阵反分解。
    Eigen::Vector3f scenePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneScale = Eigen::Vector3f::Ones();
    bool visible = true;
    // 场景实例级运行时覆盖：只影响本 World 实例，不写回 PS_*.json / SC_*.json。
    ParticleSystemRuntimeOverrides runtime;
    std::vector<EmitterSlot> emitterSlots;
    // 稳定槽位身份只增不减；数量上限仍是 kMaxParticleEmitters。
    uint32_t nextEmitterSlot = 0;
    ParticleSystemBudget budget;
    ParticleSystemAttachment attachment;
    std::unordered_map<std::string, float> userParameters;
    ParticleSystemTelemetry telemetry;
};

// 原生侧事件队列是“渲染器/物理稍后回填”的桥；C# 只在 GT 的下一帧 tick
// 请求里领取它，不会把托管回调传进渲染线程。
struct WorldParticleRuntimeEvent
{
    std::string systemName;
    std::string emitterName;
    std::string kind; // spawn|death|collision
    float time = 0.0f;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f normal = Eigen::Vector3f::Zero();
};

// Runtime view of the active game world. Camera, light, environment, and mesh
// object state is owned by World. Object GPU resources and descriptors live in
// renderer-owned backend entries and are referenced from snapshots by key.
class World
{
public:
    World(uint64_t generation, std::string scenePath);

    uint64_t GetGeneration() const { return generation; }
    const std::string& GetScenePath() const { return scenePath; }
    bool IsNativeGameplay() const { return nativeGameplay; }
    void SetNativeGameplay(bool value) { nativeGameplay = value; }
    const ProjectileSettings& GetProjectileSettings() const
    {
        return projectileSettings;
    }
    void SetProjectileSettings(ProjectileSettings value)
    {
        projectileSettings = std::move(value);
    }

    void SetViewTarget(std::shared_ptr<SceneNode> viewTarget);
    std::weak_ptr<SceneNode> GetViewTarget() const { return viewTarget; }

    void SetCamera(std::shared_ptr<Camera> camera);
    const std::shared_ptr<Camera>& GetCamera() const { return camera; }
    // 场景 Camera 是作者对象；编辑器导航使用独立观察相机，不反写作者位姿。
    const std::shared_ptr<Camera>& GetRenderCamera() const
    { return editorViewEnabled && editorCamera ? editorCamera : (scriptCamera ? scriptCamera : camera); }
    void UpdateScriptCamera(const nlohmann::json& states);
    void ClearScriptCamera() { scriptCamera.reset(); }
    void SetEditorViewEnabled(bool enabled);
    bool IsEditorViewEnabled() const { return editorViewEnabled; }

    void SetEnvironment(WorldEnvironment environment);
    const WorldEnvironment& GetEnvironment() const { return environment; }

    // 场景全局风；与 environment 同属一次 World 构建，随 World 的发布/替换整体切换。
    void SetWind(WorldWind wind);
    const WorldWind& GetWind() const { return wind; }
    // 头发分组预览是 World 的会话状态，不写资产；快照构建时冻结到每个 mesh draw。
    void SetTressfxGroupPreview(TressfxGroupPreviewState state)
    {
        tressfxGroupPreview = state;
    }
    const TressfxGroupPreviewState& GetTressfxGroupPreview() const
    {
        return tressfxGroupPreview;
    }

    void SetCsmSettings(CsmSettings settings);
    const CsmSettings& GetCsmSettings() const { return csmSettings; }

    void AddMeshObject(std::string name, WorldMeshObject object);
    void AddDecal(std::string name, WorldDecal decal);
    void AddParticleSystem(std::string name, WorldParticleSystem system);
    // 运行时 spawn/destroy 只改逻辑 World；同名 spawn 覆盖既有对象，
    // 被替换/移除的 GPU 池由下一次 Render 的 ParticleRenderer::Synchronize 退休。
    bool RemoveParticleSystem(const std::string& name);
    const WorldParticleSystem* FindParticleSystem(const std::string& name) const;
    // 粒子预览的原地更新：只替换匹配资产路径的作者数据，不动摆放/相机/环境，
    // 也不推进 World 代数；渲染快照下一帧自然读取新资产。
    size_t UpdateParticleSystemAsset(const std::string& assetPath,
        ParticleSystemAsset asset);
    void AddWindResponseProfile(WindResponseProfile profile);
    void AddSoftBodyObject(std::string name, WorldSoftBodyObject object);
    void AddGlassObject(std::string name, WorldGlassObject object);
    bool RemoveMeshObject(const std::string& name);
    bool SetMeshObjectLocalBounds(const std::string& name,
        const Eigen::Vector3f& minimum, const Eigen::Vector3f& maximum);
    bool SetMeshObjectModel(const std::string& name, const Eigen::Matrix4f& model);
    void AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light);
    void AddPointLight(std::string name, std::shared_ptr<PointLight> light);
    void AddSpotLight(std::string name, std::shared_ptr<SpotLight> light);
    void AddSquareAreaLight(std::string name, WorldSquareAreaLight light);
    // 地形碰撞摆放是 World 的只读加载期数据：WorldBuilder 从场景计划一次性写入，
    // Play 时 ScriptPhysicsWorld 读取它创建静态 heightfield；渲染路径不消费。
    void AddTerrainCollisionInstance(TerrainCollisionInstance instance);
    // 地形编辑提交：只命中指定 placement 的 chunk（单层名字无 ::lodN，
    // lodLevel 必须为 0）。多层 LOD0 同时更新共享的组包围盒。
    bool UpdateTerrainChunkBounds(const std::string& placementId,
        uint32_t chunkX, uint32_t chunkZ, uint32_t lodLevel,
        const Eigen::Vector3f& localBoundsMin,
        const Eigen::Vector3f& localBoundsMax);
    // 更新下一次 Play 重建 Jolt heightfield 用的不可变高度图快照。
    bool UpdateTerrainPlacementHeightmap(const std::string& placementId,
        std::shared_ptr<const HostImage> heightmap);
    // 编辑器 paint 提交：保留当前覆盖层快照，供 Play/Stop 或切换 Actor 后
    // 重建 TerrainPaintSession 时恢复；不影响下一次 Play 的碰撞高度图。
    bool UpdateTerrainPlacementLayerPaint(const std::string& placementId,
        std::shared_ptr<const TerrainLayerPaintData> paint);

    const std::unordered_map<std::string, WorldMeshObject>& GetMeshObjects() const { return meshObjects; }
    const std::unordered_map<std::string, WorldDecal>& GetDecals() const { return decals; }
    const std::unordered_map<std::string, WorldParticleSystem>& GetParticleSystems() const { return particleSystems; }
    const std::vector<TerrainCollisionInstance>& GetTerrainCollisionInstances() const
    { return terrainCollisionInstances; }
    const std::unordered_map<std::string, WorldSoftBodyObject>& GetSoftBodyObjects() const { return softBodyObjects; }
    const std::unordered_map<std::string, WorldGlassObject>& GetGlassObjects() const { return glassObjects; }
    void TickAnimations(float deltaSeconds);
    // 预览动作由 GT 按 World 代数校验；播放器和姿态不会传入 UI 回调。
    bool ControlAnimation(uint64_t expectedGeneration, const std::string& command, float value);
    // 只控制指定脚本网格的唯一播放器；0=seek（暂停后定位），1=play（恢复）。
    bool ControlScriptAnimation(uint64_t expectedGeneration, const std::string& identity,
        int action, float value);
    void SetMotionDebugEnabled(bool enabled) { motionDebugEnabled = enabled; }
    bool IsMotionDebugEnabled() const { return motionDebugEnabled; }
    std::shared_ptr<const Animation::MotionDebugView> CaptureMotionDebug() const;
    // P7 查询回放：index<0 退出回放，否则选择最近历史快照并暂停被聚焦播放器。
    void SetMotionDebugReplayFrame(int frame);
    int MotionDebugReplayFrame() const { return motionDebugReplayFrame; }
    Debug::DrawDebug& GetDebugDraw() { return debugDraw; }
    const Debug::DrawDebug& GetDebugDraw() const { return debugDraw; }
    std::shared_ptr<const Debug::Snapshot> CaptureDebugDraw(const Animation::MotionDebugView& motion);
    const std::unordered_map<std::string, WindResponseProfile>& GetWindResponseProfiles() const { return windResponseProfiles; }
    const std::unordered_map<std::string, std::shared_ptr<DirectionalLight>>& GetDirectionalLights() const { return directionalLights; }
    const std::shared_ptr<DirectionalLight>& GetPrimaryDirectionalLight() const;
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& GetPointLights() const { return pointLights; }
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& GetSpotLights() const { return spotLights; }
    const std::unordered_map<std::string, WorldSquareAreaLight>& GetSquareAreaLights() const { return squareAreaLights; }
    const std::unordered_map<std::string, LightSnapshot>& GetScriptLights() const { return scriptLights; }

    // 逐对象可见性写入端：只有 RuntimeCommandExecutor 在命令执行边界调用，
    // UI 回调不持有 World。名字不存在时返回 false，由调用方给出诊断而不是静默忽略。
    // 这些入口就是"哪些 World 内容可被隐藏"的唯一清单；新增可隐藏内容时
    // 必须同时补上这里、runtimeCommandExecutor 的路由和渲染快照的过滤。
    //
    // 网格按场景对象身份（MeshSceneObjectIdentity）写入，不是按 section 键：
    // 多 section 模型在 meshObjects 里是多个条目，identity 本身不是任何一个条目的键，
    // 因此"一个 actor 一个可见性"要求这里一次写全该 identity 的所有 section。
    // scene JSON 的 type|name 才是唯一身份；World 内部仍按 isTerrain 分开写，
    // 防止同名的普通 mesh 与地形 actor 互相串写。
    bool SetMeshSceneObjectVisible(const std::string& sceneObjectIdentity, bool visible);
    bool SetTerrainSceneObjectVisible(const std::string& sceneObjectIdentity, bool visible);
    bool SetLightVisible(const std::string& name, bool visible);
    // 方形面光在 World 里按值保存（不是 shared_ptr），因此单独一个入口。
    bool SetSquareAreaLightVisible(const std::string& name, bool visible);
    // 粒子系统隐藏时不从快照剔除：发射器池必须按名字继续存在并冻结，
    // 重新显示后接着原模拟状态，而不是重建/重播粒子。
    bool SetParticleSystemVisible(const std::string& name, bool visible);
    // 调用方已校验有限 TRS 与非零 scale；按 scene identity 更新所有 section。
    bool SetMeshSceneObjectTransform(const std::string& identity,
        const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
        const Eigen::Vector3f& scale);
    // 返回 false 表示名字不存在或 scale 非法（任一轴 |scale| < 1e-6 或非有限值）。
    // 粒子没有导入节点，因此直接用新 TRS 重算 model。
    bool SetParticleSystemTransform(const std::string& name,
        const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
        const Eigen::Vector3f& scale);
    // 运行时覆盖写入端：先完整校验，失败不修改；成功只改匹配实例。
    bool SetParticleSystemSpawnRateScale(const std::string& name, float scale);
    bool SetParticleSystemColorTint(const std::string& name,
        const Eigen::Vector4f& colorTint);
    bool SetParticleSystemEmitterEnabled(const std::string& name,
        uint32_t emitterIndex, bool enabled);
    bool ResetParticleSystemRuntimeOverrides(const std::string& name);
    // 发射器级运行时 API。index 使用稳定槽位身份；name 是槽位显示名。
    bool SpawnParticleEmitter(const std::string& systemName,
        uint32_t sourceStableIndex, const std::string& requestedName,
        uint32_t& outStableIndex);
    bool DestroyParticleEmitter(const std::string& systemName,
        uint32_t stableIndex);
    bool RestartParticleEmitter(const std::string& systemName,
        uint32_t stableIndex);
    bool FindParticleEmitterIndex(const std::string& systemName,
        const std::string& emitterName, uint32_t& outStableIndex) const;
    // 预算覆盖：maxTotalParticles=0 表示不设上限；scale 范围 [0,16]。
    bool SetParticleSystemBudget(const std::string& name,
        uint32_t maxTotalParticles, float emitterBudgetScale);
    bool SetWorldParticleBudget(uint32_t maxTotalParticles,
        float emitterBudgetScale);
    const ParticleSystemBudget& GetWorldParticleBudget() const
    { return particleBudget; }
    // 把 World 总量预算按名字稳定分配后，返回某个实例的实际预算。
    // 实例显式 maxTotalParticles 优先，并允许超过 World 总量。
    ParticleSystemBudget GetEffectiveParticleBudget(
        const std::string& name) const;
    // 命名用户参数：最多 64 个，key 1..64 字符，value 必须有限且 |v|<=1e6。
    bool SetParticleSystemUserParameter(const std::string& name,
        const std::string& key, float value);
    bool ClearParticleSystemUserParameter(const std::string& name,
        const std::string& key);
    // 全局命名类型化通道：最多 64 项，key 1..64 UTF-8 字节，所有组件有限且
    // |v|<=1e6。float 重载保留为 Float 类型的便捷入口。
    // 它们不属于任何 Blueprint owner，也不与逐实例 userParameters 合并存储；
    // 快照按帧冻结副本，渲染器再决定 `$Name` 的解析优先级。
    bool SetParticleDataChannel(const std::string& name, float value);
    bool SetParticleDataChannel(const std::string& name,
        const ParticleDataChannelValue& value);
    // 返回 false 表示 key 非法或当前不存在；不会为 clear 隐式创建条目。
    bool ClearParticleDataChannel(const std::string& name);
    // 仅标量类型（Float/Int/Bool）可读为 float；向量/四元数返回 false。
    bool GetParticleDataChannel(const std::string& name,
        float& outValue) const;
    bool GetParticleDataChannelValue(const std::string& name,
        ParticleDataChannelValue& outValue) const;
    const ParticleDataChannelMap&
        GetParticleDataChannels() const
    { return particleDataChannels; }
    size_t GetParticleDataChannelCount() const
    { return particleDataChannels.size(); }
    // 附着到网格场景对象或 Blueprint 组件；目标当前不存在时仍允许设置，
    // 快照 telemetry/attachment.resolved 会显式为 false。
    bool AttachParticleSystem(const std::string& name,
        const std::string& targetKind, const std::string& targetName,
        const std::string& componentName,
        const Eigen::Vector3f& localPosition,
        const Eigen::Vector3f& localRotation,
        const Eigen::Vector3f& localScale);
    bool DetachParticleSystem(const std::string& name);
    // 渲染器回填入口：alive 数量按 stableIndex 对齐；越界/缺失保持未知。
    bool SetParticleSystemRendererTelemetry(const std::string& name,
        uint32_t aliveParticles,
        const std::vector<uint32_t>& emitterAliveParticles,
        uint32_t lastFrameBudget);
    const ParticleSystemTelemetry* GetParticleSystemTelemetry(
        const std::string& name) const;
    nlohmann::json DescribeParticleSystemTelemetry(
        const std::string& name) const;
    // 原生事件可由渲染器/物理在 GT 上调用；队列上限 256，GT 下一帧领取。
    bool QueueParticleRuntimeEvent(const WorldParticleRuntimeEvent& event);
    std::vector<WorldParticleRuntimeEvent> TakeParticleRuntimeEvents();
    // C# 粒子订阅是事件回读的唯一消费者。owner 级计数保证任一 owner 的
    // destroy/EndPlay 只移除自己的兴趣，不会误停其它 owner 的事件采集。
    bool SetParticleEventCaptureInterest(const std::string& owner, bool enabled);
    bool IsParticleEventCaptureActive() const
    { return !particleEventCaptureOwners.empty(); }
    uint32_t ParticleEventCaptureInterestCount() const
    { return static_cast<uint32_t>(particleEventCaptureOwners.size()); }
    void ClearParticleEventCaptureInterest()
    { particleEventCaptureOwners.clear(); }
    // 每帧在快照构建时调用；没有附着时返回作者 model。
    Eigen::Matrix4f ResolveParticleSystemModel(
        const WorldParticleSystem& system, bool& resolved) const;
    bool SetTerrainSceneObjectTransform(const std::string& identity,
        const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
        const Eigen::Vector3f& scale);
    // 只读读回该 actor 的根 TRS（度数与场景 JSON 语义一致）。同一 identity 的所有
    // section 共享 scene TRS，因此取第一个命中项即可；返回 false 表示身份不在当前
    // World。Console 的增量位移/sweep 起点用它读当前 placement，不复制 World 表。
    bool GetMeshSceneObjectTransform(const std::string& identity,
        Eigen::Vector3f& position, Eigen::Vector3f& rotation,
        Eigen::Vector3f& scale) const;
    bool SetSquareAreaLight(const std::string& name, const WorldSquareAreaLight& light);

    // 读取端：与普通 mesh actor 行同一条规则——只要还有可见 section 就算可见。
    // 地形行使用自己的 chunk 快照，不通过该入口读取。
    bool IsMeshSceneObjectVisible(const std::string& sceneObjectIdentity) const;
    // 作者声明与执行上下文都属于 World，渲染快照不携带托管实例或其指针。
    void SetScriptBlueprints(nlohmann::json actors) { scriptBlueprints = std::move(actors); }
    const nlohmann::json& GetScriptBlueprints() const { return scriptBlueprints; }
    void SetScriptBlueprintSources(nlohmann::json actors) { scriptBlueprintSources = std::move(actors); }
    const nlohmann::json& GetScriptBlueprintSources() const { return scriptBlueprintSources; }
    const nlohmann::json& GetScriptBlueprintStates() const { return scriptBlueprintStates; }
    // 仅 GT 脚本调度修改当前运行值；UI/RT 继续消费冻结快照，不能持有此引用。
    nlohmann::json& ScriptBlueprintStatesForUpdate() { return scriptBlueprintStates; }
    void SetScriptBlueprintStates(nlohmann::json states) { scriptBlueprintStates = std::move(states); }
    void SetScriptContext(std::shared_ptr<ScriptWorldContext> context) { scriptContext = std::move(context); }
    const std::shared_ptr<ScriptWorldContext>& GetScriptContext() const { return scriptContext; }
    nlohmann::json ScriptAnimationCatalog() const;
    // catalog 的资产段（每个 modelPath 的 curves/montages）；与上面的 clip id 列表分开发布，
    // 避免改变旧字段形状，也便于只把有曲线的 clip 放进一次性 payload。
    nlohmann::json ScriptAnimationAssets() const;
    nlohmann::json ScriptAnimationFeedback() const;
    void ApplyScriptAnimationRequests(const nlohmann::json& actor, const nlohmann::json& requests);
    void TickScriptAnimations(float deltaSeconds, const Animation::GroundProbe* ground = nullptr);
    void ResetScriptAnimations();
    void SetMotionResourceRoot(std::string root) { motionResourceRoot = std::move(root); }
    bool SetScriptBlueprintTransform(const std::string& id, const std::string& field,
        uint32_t component, float value);
    void ApplyScriptBlueprintVisuals(const nlohmann::json& actor, bool visible = true);
    void CloneScriptBlueprintMeshes(const std::string& templateId, const nlohmann::json& actor);
    // removeTemplateEntries=false 供 SynchronizeScriptComponentMeshes 内部使用：
    // 重建同一个 actor 的网格时不能把它自己的模板条目一并删掉。
    void RemoveScriptBlueprintMeshes(const std::string& id, bool removeTemplateEntries = true);
    void PrepareScriptComponentMeshTemplates();
    void SynchronizeScriptComponentMeshes(const nlohmann::json& actor);
    void SetScriptTemplates(nlohmann::json templates) { scriptTemplates = std::move(templates); }
    const nlohmann::json& GetScriptTemplates() const { return scriptTemplates; }
    void SetScriptAssetDigests(nlohmann::json digests) { scriptAssetDigests = std::move(digests); }
    const nlohmann::json& GetScriptAssetDigests() const { return scriptAssetDigests; }
    void SetScriptResolvedBlueprints(nlohmann::json actors) { scriptResolvedBlueprints = std::move(actors); }
    const nlohmann::json& GetScriptResolvedBlueprints() const { return scriptResolvedBlueprints; }
    std::unique_ptr<ScriptTemplateExtension> PrepareScriptTemplateExtension(
        const std::string& path, const std::string& digest, const nlohmann::json& declaration,
        const std::vector<WorldMeshObject>& meshes,
        const std::vector<WindResponseProfile>& profiles) const;
    std::unique_ptr<ScriptTemplateExtension> PrepareScriptTemplateRemoval(const std::string& path) const;
    void CommitScriptTemplateExtension(std::unique_ptr<ScriptTemplateExtension> extension) noexcept;
    void SwapScriptConfiguration(nlohmann::json& actors, nlohmann::json& templates,
        std::shared_ptr<ScriptWorldContext>& context) noexcept
    {
        scriptBlueprints.swap(actors);
        scriptTemplates.swap(templates);
        scriptContext.swap(context);
    }

private:
    friend struct ScriptTemplateExtension;
    void AppendScriptComponentMeshTemplate(const nlohmann::json& blueprint);
    void RecordScriptMotion(float deltaSeconds);
    // 对属于同一个场景对象身份、且地形标志匹配的每个 mesh section 施加同一操作。
    // 身份推导只有 MeshSceneObjectIdentity() 一份，读写两端共用；terrain 过滤则
    // 区分同名的普通 mesh 与地形 actor。
    template <typename ApplyFn>
    bool ForEachMeshObjectInSceneObject(
        const std::string& sceneObjectIdentity, bool terrain, ApplyFn&& apply);
    bool SetSceneObjectTransform(const std::string& identity, bool terrain,
        const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
        const Eigen::Vector3f& scale);

    uint64_t generation = 0;
    std::string scenePath;
    // 无 C# Play 状态的 native 试玩场景；编辑器场景保持 authoring。
    bool nativeGameplay = false;
    ProjectileSettings projectileSettings;
    std::weak_ptr<SceneNode> viewTarget;
    std::shared_ptr<Camera> camera;
    std::shared_ptr<Camera> editorCamera;
    std::shared_ptr<Camera> scriptCamera;
    bool editorViewEnabled = false;
    WorldEnvironment environment;
    WorldWind wind;
    TressfxGroupPreviewState tressfxGroupPreview;
    CsmSettings csmSettings;
    std::unordered_map<std::string, WorldMeshObject> meshObjects;
    std::unordered_map<std::string, WorldDecal> decals;
    std::unordered_map<std::string, WorldParticleSystem> particleSystems;
    ParticleSystemBudget particleBudget;
    // 全局通道不随 Blueprint 上下文生命周期增删；Play/Stop 与热重载只替换
    // 脚本 owner，World 仍是同一实例，因此通道状态必须留在 World 上。
    ParticleDataChannelMap particleDataChannels;
    std::vector<WorldParticleRuntimeEvent> particleRuntimeEvents;
    std::unordered_set<std::string> particleEventCaptureOwners;
    std::unordered_map<std::string, WindResponseProfile> windResponseProfiles;
    std::unordered_map<std::string, WorldSoftBodyObject> softBodyObjects;
    std::unordered_map<std::string, WorldGlassObject> glassObjects;
    std::unordered_map<std::string, std::shared_ptr<DirectionalLight>> directionalLights;
    std::string primaryDirectionalLightName;
    std::unordered_map<std::string, std::shared_ptr<PointLight>> pointLights;
    std::unordered_map<std::string, std::shared_ptr<SpotLight>> spotLights;
    std::unordered_map<std::string, WorldSquareAreaLight> squareAreaLights;
    // 每个 terrain placement 一份；同一 TR 多摆放按 placementId 区分。
    std::vector<TerrainCollisionInstance> terrainCollisionInstances;
    nlohmann::json scriptBlueprints = nlohmann::json::array();
    nlohmann::json scriptBlueprintSources = nlohmann::json::array();
    // 运行时 TRS 与作者声明分离；逻辑 Actor 没有网格时仍能在 Details 回显。
    nlohmann::json scriptBlueprintStates = nlohmann::json::array();
    std::shared_ptr<ScriptWorldContext> scriptContext;
    nlohmann::json scriptTemplates = nlohmann::json::object();
    nlohmann::json scriptAssetDigests = nlohmann::json::object();
    // 保留补齐类默认值前的已解析作者声明；程序集重载必须重新继承新默认值。
    nlohmann::json scriptResolvedBlueprints = nlohmann::json::array();
    std::unordered_map<std::string, Eigen::Matrix4f> scriptComponentMatrices;
    std::unordered_map<std::string, std::vector<WorldMeshObject>> scriptComponentMeshTemplates;
    std::unordered_map<std::string, std::string> scriptComponentMeshStructures;
    // 运行时 spawn 创建的逐 actor 模板键：模板键对 tressfx 组件包含父组件身份，
    // 因此每个 spawn 都有新键；actor 销毁时按这张表回收，避免模板池随 spawn 增长。
    // 共享网格的模板键是 actor 无关的，不会记录在这里。
    std::unordered_map<std::string, std::vector<std::string>> scriptActorTemplateKeys;
    // 派生组件灯只持有规范物理值，不进入作者灯表，也不替换 CSM 主方向光。
    std::unordered_map<std::string, LightSnapshot> scriptLights;
    bool motionDebugEnabled = false;
    // 最近 10 秒（60 Hz）的冻结调试视图；只由 GT 写，UI 读副本。
    mutable std::deque<Animation::MotionDebugView> motionDebugHistory;
    mutable int motionDebugReplayFrame = -1;
    Debug::DrawDebug debugDraw;
    std::map<std::string, Animation::MotionTrack> motionTracks;
    std::map<std::string, Animation::MotionRuntime> motionRuntimes;
    // 每个 World 一份同步组注册表；每 tick 重建参与者并复用容量，不持有播放器
    // 所有权，也不跨 World 换代保存旧指针。
    Animation::AnimationSyncGroupRegistry syncGroupRegistry;
    // 缓存 key 含源路径、模型路径及实际导入输入身份；Stop 只清实例时钟/历史，保留不可变资源。
    std::map<std::string, std::shared_ptr<const Animation::MotionRuntimeDatabase>> motionDatabases;
    // PoseDriver 资产与骨架解耦：同一份资产可按需绑定到任意兼容 SKM，按路径缓存摘要。
    std::map<std::string, std::shared_ptr<const Animation::PoseDriverSet>> poseDriverSets;
    // PostProcess（Twist/Helpers）资产同样按路径缓存，不与 SKM 绑定。
    std::map<std::string, std::shared_ptr<const Animation::PostProcessAsset>> postProcessAssets;
    // 骨骼物理：资产按路径缓存，solver 按 mesh identity 持有（Jolt 世界独立）。
    std::map<std::string, std::shared_ptr<const Physics::BonePhysicsAsset>> bonePhysicsAssets;
    std::map<std::string, std::shared_ptr<Physics::BonePhysicsSolver>> bonePhysicsSolvers;
    // 只有 C# 请求时才把 21 个刚体描述塞进反馈，避免每帧无谓的 JSON 体积。
    std::map<std::string, bool> bonePhysicsDebug;
    std::string motionResourceRoot;
};

} // namespace VL
