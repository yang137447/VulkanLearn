#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include <Eigen/Dense>

#include "baseStructs.h"
#include "environmentType.h"
#include "mesh/meshAssetTypes.h"
#include "render/shadow/csmSettings.h"
#include "world/worldSnapshot.h"
#include "animation/motionDebug.h"
#include "animation/motionHistory.h"
#include "animation/motionRuntime.h"
#include "debug/drawDebug.h"
#include <map>

class Camera;
class DirectionalLight;
class PointLight;
class SceneNode;
class SpotLight;

namespace VL
{
namespace Animation { class SkeletalPlayback; struct SkinPose; }
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
};

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
};

// 场景里声明的橡皮软体对象。它只保存作者参数和已解析的材质 key；
// RubberShell 粒子、动态网格和爆裂状态由 GT 侧运行时持有，不进入 World。
struct WorldSoftBodyObject
{
    std::string name;
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

// Runtime view of the active game world. Camera, light, environment, and mesh
// object state is owned by World. Object GPU resources and descriptors live in
// renderer-owned backend entries and are referenced from snapshots by key.
class World
{
public:
    World(uint64_t generation, std::string scenePath);

    uint64_t GetGeneration() const { return generation; }
    const std::string& GetScenePath() const { return scenePath; }

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

    void SetCsmSettings(CsmSettings settings);
    const CsmSettings& GetCsmSettings() const { return csmSettings; }

    void AddMeshObject(std::string name, WorldMeshObject object);
    void AddSoftBodyObject(std::string name, WorldSoftBodyObject object);
    void AddGlassObject(std::string name, WorldGlassObject object);
    bool RemoveMeshObject(const std::string& name);
    bool SetMeshObjectLocalBounds(const std::string& name,
        const Eigen::Vector3f& minimum, const Eigen::Vector3f& maximum);
    bool SetMeshObjectModel(const std::string& name, const Eigen::Matrix4f& model);
    void AddSpeedTreeWindProfile(SpeedTreeWindProfile profile);
    void AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light);
    void AddPointLight(std::string name, std::shared_ptr<PointLight> light);
    void AddSpotLight(std::string name, std::shared_ptr<SpotLight> light);
    void AddSquareAreaLight(std::string name, WorldSquareAreaLight light);

    const std::unordered_map<std::string, WorldMeshObject>& GetMeshObjects() const { return meshObjects; }
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
    const std::unordered_map<std::string, SpeedTreeWindProfile>& GetSpeedTreeWindProfiles() const { return speedTreeWindProfiles; }
    const std::unordered_map<std::string, std::shared_ptr<DirectionalLight>>& GetDirectionalLights() const { return directionalLights; }
    const std::shared_ptr<DirectionalLight>& GetPrimaryDirectionalLight() const;
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& GetPointLights() const { return pointLights; }
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& GetSpotLights() const { return spotLights; }
    const std::unordered_map<std::string, WorldSquareAreaLight>& GetSquareAreaLights() const { return squareAreaLights; }
    const std::unordered_map<std::string, LightSnapshot>& GetScriptLights() const { return scriptLights; }

    // 逐对象可见性写入端：只有 RuntimeCommandExecutor 在命令执行边界调用，
    // UI 回调不持有 World。名字不存在时返回 false，由调用方给出诊断而不是静默忽略。
    // 这里的四个入口就是"哪些 World 内容可被隐藏"的唯一清单；新增可隐藏内容时
    // 必须同时补上这里、runtimeCommandExecutor 的路由和渲染快照的过滤。
    //
    // 网格按场景对象身份（MeshSceneObjectIdentity）写入，不是按 section 键：
    // 多 section 模型在 meshObjects 里是多个条目，identity 本身不是任何一个条目的键，
    // 因此"一个 actor 一个可见性"要求这里一次写全该 identity 的所有 section。
    bool SetMeshSceneObjectVisible(const std::string& sceneObjectIdentity, bool visible);
    bool SetLightVisible(const std::string& name, bool visible);
    // 方形面光在 World 里按值保存（不是 shared_ptr），因此单独一个入口。
    bool SetSquareAreaLightVisible(const std::string& name, bool visible);
    // 调用方已校验有限 TRS 与非零 scale；按 scene identity 更新所有 section。
    bool SetMeshSceneObjectTransform(const std::string& identity,
        const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
        const Eigen::Vector3f& scale);
    bool SetSquareAreaLight(const std::string& name, const WorldSquareAreaLight& light);

    // 读取端：与 scene/editorSceneActorBuilder 的 actor 行同一条规则——
    // 只要还有可见 section 就算可见。两个入口共用同一份 identity 推导，
    // 因此"行显示什么"与"点击会写什么"不会漂移。
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
    void RemoveScriptBlueprintMeshes(const std::string& id);
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
        const std::vector<SpeedTreeWindProfile>& profiles) const;
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
    // 对属于同一个场景对象身份的每个 mesh section 施加同一操作，返回是否至少命中一个。
    // 身份推导只有 MeshSceneObjectIdentity() 一份，读写两端共用，避免"大纲认得的名字
    // World 不认得"这类静默失配。
    template <typename ApplyFn>
    bool ForEachMeshObjectInSceneObject(const std::string& sceneObjectIdentity, ApplyFn&& apply);

    uint64_t generation = 0;
    std::string scenePath;
    std::weak_ptr<SceneNode> viewTarget;
    std::shared_ptr<Camera> camera;
    std::shared_ptr<Camera> editorCamera;
    std::shared_ptr<Camera> scriptCamera;
    bool editorViewEnabled = false;
    WorldEnvironment environment;
    CsmSettings csmSettings;
    std::unordered_map<std::string, WorldMeshObject> meshObjects;
    std::unordered_map<std::string, WorldSoftBodyObject> softBodyObjects;
    std::unordered_map<std::string, WorldGlassObject> glassObjects;
    std::unordered_map<std::string, SpeedTreeWindProfile> speedTreeWindProfiles;
    std::unordered_map<std::string, std::shared_ptr<DirectionalLight>> directionalLights;
    std::string primaryDirectionalLightName;
    std::unordered_map<std::string, std::shared_ptr<PointLight>> pointLights;
    std::unordered_map<std::string, std::shared_ptr<SpotLight>> spotLights;
    std::unordered_map<std::string, WorldSquareAreaLight> squareAreaLights;
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
    // 派生组件灯只持有规范物理值，不进入作者灯表，也不替换 CSM 主方向光。
    std::unordered_map<std::string, LightSnapshot> scriptLights;
    bool motionDebugEnabled = false;
    // 最近 10 秒（60 Hz）的冻结调试视图；只由 GT 写，UI 读副本。
    mutable std::deque<Animation::MotionDebugView> motionDebugHistory;
    mutable int motionDebugReplayFrame = -1;
    Debug::DrawDebug debugDraw;
    std::map<std::string, Animation::MotionTrack> motionTracks;
    std::map<std::string, Animation::MotionRuntime> motionRuntimes;
    // 缓存 key 含源路径、模型路径及实际导入输入身份；Stop 只清实例时钟/历史，保留不可变资源。
    std::map<std::string, std::shared_ptr<const Animation::MotionRuntimeDatabase>> motionDatabases;
    std::string motionResourceRoot;
};

} // namespace VL
