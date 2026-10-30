#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "../mesh/loader/common/meshAssetLoader.h"

namespace VL::Animation { class SkeletalPlayback; struct SkinPose; }

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
    std::string meshKey;
    std::string materialKey;
    std::string materialInstanceKey;
    std::string speedTreeWindProfileKey;
};

// 运行时橡皮软体对象的纯数据计划。渲染资源加载阶段只加载材质并记录 key；
// 实际 RubberShell 与动态 RenderableObject 由 GT 侧 SoftBodyRuntime 创建。
struct SoftBodyObjectBuildPlan
{
    std::string objectName;
    std::string sceneObjectIdentity;
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
};

// 运行时玻璃板的纯数据计划。完整板、碎片和二次碎裂都在运行时生成；
// 这里只记录作者参数、TRS 和已解析的材质 key。
struct GlassObjectBuildPlan
{
    std::string objectName;
    std::string sceneObjectIdentity;
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
    // 裂纹覆盖层专用深色 MI；为空时 cracked 只保留 debug 折线。
    std::string crackMaterialKey;
    std::string crackMaterialInstanceKey;
};
