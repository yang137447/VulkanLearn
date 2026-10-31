#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "../mesh/loader/common/meshAssetLoader.h"
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
    std::string windResponseProfileKey;
    // TressFX guide 中心线点数；顶点工厂用它从 gl_VertexIndex 反推 guideIndex。
    // 非头发对象为 0。
    uint32_t tressfxPointsPerGuide = 0;
    VL::PlacementBinding placementBinding;
};
