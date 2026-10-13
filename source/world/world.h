#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <Eigen/Dense>

#include "baseStructs.h"
#include "environmentType.h"
#include "mesh/meshAssetTypes.h"
#include "render/shadow/csmSettings.h"

class Camera;
class DirectionalLight;
class PointLight;
class SceneNode;
class SpotLight;

namespace VL
{

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
    std::string meshKey;
    std::string materialKey;
    std::string materialInstanceKey;
    std::string speedTreeWindProfileKey;
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
    { return editorViewEnabled && editorCamera ? editorCamera : camera; }
    void SetEditorViewEnabled(bool enabled);
    bool IsEditorViewEnabled() const { return editorViewEnabled; }

    void SetEnvironment(WorldEnvironment environment);
    const WorldEnvironment& GetEnvironment() const { return environment; }

    void SetCsmSettings(CsmSettings settings);
    const CsmSettings& GetCsmSettings() const { return csmSettings; }

    void AddMeshObject(std::string name, WorldMeshObject object);
    void AddSpeedTreeWindProfile(SpeedTreeWindProfile profile);
    void AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light);
    void AddPointLight(std::string name, std::shared_ptr<PointLight> light);
    void AddSpotLight(std::string name, std::shared_ptr<SpotLight> light);
    void AddSquareAreaLight(std::string name, WorldSquareAreaLight light);

    const std::unordered_map<std::string, WorldMeshObject>& GetMeshObjects() const { return meshObjects; }
    const std::unordered_map<std::string, SpeedTreeWindProfile>& GetSpeedTreeWindProfiles() const { return speedTreeWindProfiles; }
    const std::unordered_map<std::string, std::shared_ptr<DirectionalLight>>& GetDirectionalLights() const { return directionalLights; }
    const std::shared_ptr<DirectionalLight>& GetPrimaryDirectionalLight() const;
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& GetPointLights() const { return pointLights; }
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& GetSpotLights() const { return spotLights; }
    const std::unordered_map<std::string, WorldSquareAreaLight>& GetSquareAreaLights() const { return squareAreaLights; }

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

private:
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
    bool editorViewEnabled = false;
    WorldEnvironment environment;
    CsmSettings csmSettings;
    std::unordered_map<std::string, WorldMeshObject> meshObjects;
    std::unordered_map<std::string, SpeedTreeWindProfile> speedTreeWindProfiles;
    std::unordered_map<std::string, std::shared_ptr<DirectionalLight>> directionalLights;
    std::string primaryDirectionalLightName;
    std::unordered_map<std::string, std::shared_ptr<PointLight>> pointLights;
    std::unordered_map<std::string, std::shared_ptr<SpotLight>> spotLights;
    std::unordered_map<std::string, WorldSquareAreaLight> squareAreaLights;
};

} // namespace VL
