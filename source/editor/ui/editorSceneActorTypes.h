#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "world/worldSnapshot.h"

namespace VL::EditorUi
{

// 大纲 actor 的类型。前七种与场景 JSON 的对象 type 一一对应，WindProfile 是
// 网格导入产生的风剖面（不是场景对象），最后一类只表达 World 里确实存在的状态。
enum class EditorSceneActorType : uint8_t
{
    Mesh,
    DirectionalLight,
    PointLight,
    SpotLight,
    SquareAreaLight,
    Camera,
    Environment,
    WindProfile
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

// 相机只发布 Details 需要的七个字段。世界快照里的 CameraSnapshot 还带 view/projection
// 等渲染矩阵，UI 不消费它们，因此不把渲染矩阵放进编辑器视图模型。
struct EditorSceneCameraActor
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    Eigen::Vector3f right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Vector3f up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    float horizontalFovDegrees = 60.0f;
    float clipNear = 0.1f;
    float clipFar = 1000.0f;
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
// 与 SpeedTreeWindProfile 的字段同名，便于后续编辑轮直接映射回去。
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
    // 真正有渲染侧可见性；相机/环境/风剖面固定为 true，也不显示眼睛控件。
    bool visible = true;
    // 只有 Mesh 使用；其余类型为空。
    std::vector<EditorSceneMeshSectionSnapshot> sections;
    // Mesh：场景 JSON 里的对象名（多 section 共享）与模型级 RuntimeId。
    std::string sceneObjectIdentity;
    RuntimeId meshObjectId = 0;
    // 四类灯共用一个字段，type 由 light.type 区分（LightSnapshotType）。
    LightSnapshot light;
    // 主方向光 = World::GetPrimaryDirectionalLight()，SetSunIntensity 与 CSM 只作用于它。
    bool primaryDirectionalLight = false;
    EditorSceneCameraActor camera;
    EditorSceneEnvironmentActor environment;
    EditorSceneWindActor wind;
};

} // namespace VL::EditorUi
