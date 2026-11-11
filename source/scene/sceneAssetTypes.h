#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "../mesh/loader/common/meshAssetLoader.h"
#include "../terrain/terrainAssetTypes.h"
#include "scene/placementBinding.h"

namespace VL::Animation { class SkeletalPlayback; struct SkinPose; }
namespace VL::Hair { struct TfxBoneAsset; }
namespace VL::Hair { class TressfxRootCloud; }
namespace VL::Hair { struct TressfxGroupBinding; }

// One validated scene asset object entry used by WorldBuilder and renderer loaders.
// Mesh objects carry an already built mesh load request so runtime code does
// not rebuild it.
struct SceneAssetObjectPlan
{
    size_t objectIndex = 0;
    std::string objectName;
    std::string objectType;
    std::optional<MeshAssetLoadRequest> meshLoadRequest;
    // 地形对象在场景预检阶段完成 TR 解析与高度图解码；WorldBuilder 仍只消费
    // 渲染器派生的 MeshObjectBuildPlan，不引入第二个 World 地形类型。
    std::optional<VL::TerrainBuildPlan> terrainBuildPlan;
    // Actor 根网格的派生声明；作者 JSON 不增加合成对象，正式 mesh loader 仍只读网格计划。
    std::optional<nlohmann::json> componentObject;
};

// Scene-level validated loading plan consumed by WorldLoader, WorldBuilder, and
// renderer resource loaders.
struct SceneAssetBuildPlan
{
    std::string scenePath;
    std::vector<SceneAssetObjectPlan> objectPlans;
};

// One renderable mesh section expanded into World-owned object data. Renderer
// resource loading produces these entries while creating renderable/material
// resources; WorldBuilder consumes only this pure data plan, not renderer cache
// binding tables.
struct MeshObjectBuildPlan
{
    std::shared_ptr<VL::Animation::SkeletalPlayback> animation;
    std::shared_ptr<const VL::Animation::SkinPose> skinPose;
    size_t skinSection = 0;
    std::string objectName;
    // 这是场景 JSON 中的原始对象身份；objectName 可能因多 section 被展开。
    std::string sceneObjectIdentity;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    std::string debugName;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    // 与最终 model 一起发布作者根 TRS，编辑器无需分解矩阵或重新读取 scene JSON。
    Eigen::Vector3f scenePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f sceneScale = Eigen::Vector3f::Ones();
    Eigen::Vector3f localBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f localBoundsMax = Eigen::Vector3f::Zero();
    // 蓝图碰撞复用正式导入的 section 局部三角；候选/World/Spawn 克隆共享不可变 CPU 数据。
    std::shared_ptr<const std::vector<Eigen::Vector3f>> collisionTriangles;
    // 可选 .tfxbone 解析结果，供加载期映射到目标骨架；不在运行时读文件。
    std::shared_ptr<const VL::Hair::TfxBoneAsset> tressfxBoneAsset;
    // TressFX 发根选择缓存（model/head-local）。与导入 section 共享只读 ptr；
    // 编辑器按当前 object.model 变换，不复制点云。
    std::shared_ptr<const VL::Hair::TressfxRootCloud> tressfxRootCloud;
    // 已保存资产的 guide->group 映射与组名；与导入几何分离，组编辑只换这一份。
    // 编辑器用名称把草稿组映射到已保存组号，避免删除/新建后高亮错组。
    std::shared_ptr<const VL::Hair::TressfxGroupBinding> tressfxGroupBinding;
    // placement 解析后的每组配置（index 0 = Default，k = 第 k 个命名组）。
    // 编辑器 Groups 检查器的 asset default 读它，而不是只读文档 simulationDefaults。
    std::shared_ptr<const std::vector<TressfxSimulationConfig>> tressfxGroupConfigs;
    std::string meshKey;
    std::string materialKey;
    std::string materialInstanceKey;
    std::string speedTreeWindProfileKey;
    // 本体 section 需要为 forward 描边写 GBuffer outline mask。
    bool writesOutlineMask = false;
    // rendererTerrainLoader 显式写入：feedback pass 只绘制这些对象，
    // 不能依赖材质名/资产路径猜测地形。
    bool terrainFeedbackEligible = false;
    // 地形几何 LOD 组：同一 chunk 的各 LOD 级共享该身份，空表示无 LOD 组。
    // 快照构建器按组只输出一个网格，非地形/单级地形保持空组和旧行为。
    std::string lodGroupId;
    uint32_t lodLevel = 0;
    uint32_t lodLevelCount = 1;
    // 该组（chunk）的切换距离（cm，升序）与 LOD0 局部 AABB；所有级共用，
    // 保证距离计算不随当前级变化，避免选择反馈。
    std::vector<float> lodDistancesCm;
    Eigen::Vector3f lodGroupBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f lodGroupBoundsMax = Eigen::Vector3f::Zero();
    // 地形在 World 中仍是 chunk mesh；独立 Terrain 身份只属于编辑器
    // 展示/身份层，不引入第二套 World 地形类型。
    bool isTerrain = false;
    std::string terrainAssetPath;
    // 一次场景摆放的身份（terrain|<objectName>）。同一 TR 可摆放多次，
    // assetPath 不能作为 chunk 更新/碰撞回写的唯一键。
    std::string terrainPlacementId;
    std::string windResponseProfileKey;
    // TressFX guide 中心线点数；顶点工厂用它从 gl_VertexIndex 反推 guideIndex。
    // 非头发对象为 0。
    uint32_t tressfxPointsPerGuide = 0;
    VL::PlacementBinding placementBinding;
};

// 运行时橡皮软体对象的纯作者数据计划。这里只保存场景 JSON 的
// materialInstance 路径；实际 Material / MaterialInstance key 由渲染器资源
// 加载阶段解析成 ResolvedMaterialBinding，RubberShell 由 GT 侧 SoftBodyRuntime 创建。
struct SoftBodyObjectBuildPlan
{
    std::string objectName;
    std::string sceneObjectIdentity;
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
    std::string materialInstancePath;
};

// 运行时玻璃板的纯作者数据计划。完整板、碎片和二次碎裂都在运行时生成；
// 这里只记录作者参数、TRS 与材质路径，渲染器解析 key，WorldBuilder 合并。
struct GlassObjectBuildPlan
{
    std::string objectName;
    std::string sceneObjectIdentity;
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
    std::string materialInstancePath;
    // 裂纹覆盖层专用深色 MI 路径；为空时 cracked 只保留 debug 折线。
    std::string crackMaterialInstancePath;
};
