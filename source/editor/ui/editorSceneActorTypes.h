#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include <Eigen/Dense>

#include "world/worldSnapshot.h"

namespace VL::EditorUi
{

// 大纲 actor 的类型。Mesh..Environment 与 SceneWind 和场景 JSON 的对象 type 一一对应；
// WindProfile 是网格导入产生的每物种风剖面（不是场景对象），不要与 SceneWind 混用；
// Script 只表达 World 里确实存在的蓝图实例状态。
enum class EditorSceneActorType : uint8_t
{
    Mesh,
    DirectionalLight,
    PointLight,
    SpotLight,
    SquareAreaLight,
    Camera,
    Environment,
    WindProfile,
    SceneWind,
    Script
};

// 一个 mesh actor 的渲染子对象。World 的 meshObjects 按 section 展开，
// 多 section 模型因此有多个条目共享同一个 sceneObjectIdentity。
struct EditorSceneMeshSectionSnapshot
{
    std::string objectName;
    // 该 section 的稳定 RuntimeId（与 WorldSnapshotBuilder 同源），用于选择描边。
    RuntimeId objectId = 0;
    std::string meshKey;
    std::string materialKey;
    // 规范化 MI 资产路径；没有材质实例时为空（点击该槽不会打开任何文档）。
    std::string materialInstancePath;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    std::string windProfileKey;
};

// 相机发布 Details 的作者曝光与辅助线需要的基向量/投影参数；
// UI 不消费渲染矩阵或 GPU 自动曝光历史，只接收冻结的作者值。
struct EditorSceneCameraActor
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    Eigen::Vector3f right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Vector3f up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    float horizontalFovDegrees = 60.0f;
    float clipNear = 0.1f;
    float clipFar = 1000.0f;
    CameraExposure exposure;
};

// 正式加载后的组件 section 包围盒与完整模型矩阵；仅供编辑器拾取，不含 GPU 所有权。
struct EditorScriptComponentBounds
{
    std::string componentId;
    Eigen::Matrix4f worldFromLocal = Eigen::Matrix4f::Identity();
    Eigen::Vector3f minimum = Eigen::Vector3f::Zero();
    Eigen::Vector3f maximum = Eigen::Vector3f::Zero();
};

// EnvironmentSnapshot 只携带 GPU 侧 resource handle，没有 cubeSize/hdrPath，
// 因此环境 actor 的展示字段从 WorldEnvironment 取值后在这里冻结。
struct EditorSceneEnvironmentActor
{
    EnvironmentType type = EnvironmentType::ProceduralSky;
    uint32_t cubeSize = 128;
    float intensity = 1.0f;
    std::string hdrPath;
};

// 风剖面的曲线表（每个 20 点 × 5 条）不属于展示字段，这里只冻结标量与开关，
// 与 WindResponseProfile 的字段同名，便于后续编辑轮直接映射回去。
struct EditorSceneWindActor
{
    std::string key;
    Eigen::Vector3f sourceBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceBoundsMax = Eigen::Vector3f::Zero();
    float strengthResponse = 0.0f;
    float directionResponse = 0.0f;
    float gustFrequency = 0.0f;
    float gustStrengthMin = 0.0f;
    float gustStrengthMax = 0.0f;
    float gustDurationMin = 0.0f;
    float gustDurationMax = 0.0f;
    float gustRiseScalar = 0.0f;
    float gustFallScalar = 0.0f;
    float sharedStartHeight = 0.0f;
    float branch1StretchLimit = 0.0f;
    float branch2StretchLimit = 0.0f;
    bool doShared = false;
    bool doBranch1 = false;
    bool doBranch2 = false;
    bool doRipple = false;
    bool doShimmer = false;
};

// 场景级全局风（World::GetWind()）。全场景至多一个，没有每对象的作者名字，
// 因此身份是固定字符串而不是对象名。注意：这不是上面的 EditorSceneWindActor
//（SpeedTree 每物种风剖面），两者在大纲里是两种不同的 actor。
struct EditorSceneWorldWindActor
{
    // 已归一化的风向；长度由 strengthCmPerSec 单独表达。
    Eigen::Vector3f direction = Eigen::Vector3f::UnitX();
    float strengthCmPerSec = 0.0f;
    // 阵风是连续噪声调制，没有离散阵风事件；关闭时三个数值保持 0。
    bool gustEnabled = false;
    float gustAmplitude = 0.0f;
    float gustFrequency = 0.0f;
    float gustTurbulence = 0.0f;
};

// 冻结的只读 actor 行 + 类型化参数。UI 只接收值语义：没有 World、SceneNode 或
// GPU 对象指针，也没有渲染矩阵；灯光参数直接复用 LightSnapshot（字段与渲染快照一致）。
struct EditorSceneActorSnapshot
{
    EditorSceneActorType type = EditorSceneActorType::Mesh;
    uint64_t worldGeneration = 0;
    std::string scenePath;
    // 稳定身份（"mesh|<scene object>"、"pointLight|Key_Point_Light"、…）：跨帧选择与
    // 展开状态的键，不是 DOM 索引，也不能当作 GPU 资源身份。
    std::string identity;
    std::string displayName;
    // 逐 actor 可见性（World 侧运行时状态，非场景 JSON 字段）。只有网格与四类灯
    // 真正有渲染侧可见性；相机/环境/场景风/风剖面固定为 true，也不显示眼睛控件。
    bool visible = true;
    // 只有 Mesh 使用；其余类型为空。
    std::vector<EditorSceneMeshSectionSnapshot> sections;
    // Mesh：场景 JSON 里的对象名（多 section 共享）与模型级 RuntimeId。
    std::string sceneObjectIdentity;
    RuntimeId meshObjectId = 0;
    // 作者根 TRS：网格取 scene JSON 根，节点取 SceneNode；旋转沿用 VL 的度制 XYZ。
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f scale = Eigen::Vector3f::Ones();
    // 四类灯共用一个字段，type 由 light.type 区分（LightSnapshotType）。
    LightSnapshot light;
    // 主方向光 = World::GetPrimaryDirectionalLight()，SetSunIntensity 与 CSM 只作用于它。
    bool primaryDirectionalLight = false;
    bool castShadows = true;
    EditorSceneCameraActor camera;
    // 仅 Camera 行携带观察相机读数，供坐标轴与辅助线投影；不是第二个场景 actor。
    EditorSceneCameraActor viewportCamera;
    bool hasViewportCamera = false;
    std::string scriptClass;
    nlohmann::json scriptProperties = nlohmann::json::object();
    nlohmann::json scriptInstanceEditable = nlohmann::json::array();
    std::string scriptBlueprintPath;
    nlohmann::json scriptOverrides = nlohmann::json::object();
    nlohmann::json scriptComponentInstanceEditable = nlohmann::json::object();
    nlohmann::json scriptComponentOverrides = nlohmann::json::object();
    nlohmann::json scriptComponents = nlohmann::json::array();
    // C# 逻辑组件与可视组件父链分离；Scene 只持有获授权属性的稀疏覆写。
    nlohmann::json scriptLogicComponents = nlohmann::json::array();
    nlohmann::json scriptLogicOverrides = nlohmann::json::object();
    std::vector<EditorScriptComponentBounds> scriptComponentBounds;
    nlohmann::json scriptPhysics = nullptr;
    bool scriptSpawned = false;
    EditorSceneEnvironmentActor environment;
    EditorSceneWindActor wind;
    // 场景级全局风对象，不是 SpeedTree 每物种风剖面（上面的 wind）。
    EditorSceneWorldWindActor worldWind;
};

} // namespace VL::EditorUi
