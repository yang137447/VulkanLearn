#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "../vertexDataStruct.h"
#include "hair/sdfBuilder.h"
#include "hair/tressfxCollision.h"
#include "hair/tressfxSolverMath.h"

namespace VL::Animation { struct SkeletalAsset; }
namespace VL::Hair { struct TfxBoneAsset; }

// Result of resolving one mesh asset JSON into the effective JSON consumed by mesh validation.
// It keeps resolver output separate from runtime loading so later defaults or platform overrides can
// be introduced without moving that policy into RendererMeshLoader.
struct MeshAssetResolveResult
{
    nlohmann::json effectiveMeshAssetJson;
};

// One material slot declared by VulkanLearn mesh asset JSON.
// Slots are stored in JSON array order. The slot name is a readable config/debug label owned by
// the asset descriptor; runtime section mapping uses source material order, not this name.
struct MeshMaterialSlot
{
    std::string name;
    std::string materialInstancePath;
};

struct MeshImportOptions
{
    bool generateSmoothNormals = false;
    std::vector<std::string> includedSectionNames;
};

// TressFX guide simulation authoring. Units follow the project world contract:
// positions are centimeters and acceleration is cm/s^2.
enum class TressfxSimulationBackend
{
    // 默认路径：游戏线程运行 VL::Hair::GuideSimulation，逐帧写 host-coherent SSBO。
    Cpu,
    // GPU 路径：compute shader 独占写 SSBO，CPU 不构造也不步进参考 solver。
    Gpu,
};

// 逐 strand 根点权重的来源。RootBone 保持历史单根骨；Mesh 在加载期从源角色
// 绑定姿态网格派生多骨权重（每个 guide 根点复制最近身体顶点的完整影响）。
// 两种来源都不改 GPU 通道：多骨数据仍走 binding 14/15 与 motion.w bit2。
enum class TressfxRootWeightSource
{
    RootBone,
    Mesh,
};

// simulation.skinning 的作者声明：把发丝根点从前 N 个固定点改为跟随源角色骨骼。
// source 是同一场景里的 mesh 对象名，rootBone 是该对象目标骨架的关节名。
// 缺省（nullopt）表示保持“根点钉在 rest”的历史行为。
struct TressfxSimulationSkinning
{
    std::string source;
    std::string rootBone;
    // 可选 .tfxbone（资源仓库相对路径）。非空时导入期解析并与目标骨架映射，
    // 运行期用逐 strand 多骨权重替换单根骨 InitialPos；空表示保持单根骨行为。
    std::string boneWeightsPath;
    // 根点权重来源。Mesh 与 boneWeightsPath 互斥：一个是派生，一个是显式作者数据，
    // 同时出现会让“谁覆盖谁”变成隐式规则，描述符边界直接拒绝。
    TressfxRootWeightSource rootWeights = TressfxRootWeightSource::RootBone;
    // 作者是否显式写了 weights。蓝图 tressfx 组件挂载时只有“缺省”才自动升级为
    // mesh 派生；显式 rootBone 必须被尊重。
    bool rootWeightsAuthored = false;
};

// 放置 `hairCollision.mesh` 的作者声明。path 是资源仓库相对路径的低模 OBJ；
// cellSize 是 GPU 距离场的格子边长（厘米），margin 是头发表面与代理表面之间
// 要保持的最小距离（厘米）。运行期距离场由这份三角形汤逐帧重建，不读取
// 预烘焙的 .sdf 文件。
struct TressfxCollisionMeshConfig
{
    std::string path;
    float cellSize = 1.0f;
    float margin = 1.0f;
};

struct TressfxSimulationConfig
{
    bool enabled = false;
    Eigen::Vector3f gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    // 单个风向量：方向归一化后作为风锥轴，长度作为 TressFX WindMagnitude
    // 力标量，不是物理风速。
    Eigen::Vector3f wind = Eigen::Vector3f::Zero();
    // TressFX 的 DampingCoeff，直接用于 exp(-damping*dt*60)；
    // 典型 0..0.07，默认 0.035，不是“每步保留的速度比例”。
    float damping = 0.035f;
    uint32_t iterations = 3;
    uint32_t rootFixedPoints = 2;
    // 仅为兼容既有资产保留：GPU 原生调度与 CPU StepFrame 使用源固定
    // 0.0125 s/substep，不再读 maxDt；只有旧的可变步长入口
    // GuideSimulation::Step 仍按它 clamp。资产值仍限制在 (0, 0.016]。
    float maxDt = VL::Hair::TressfxMaxTimeStepSeconds;
    // 映射到 LocalShape pass 的 Stiffness 标量；pass 内再乘 0.5（TressFX 公式）。
    // 0 关闭局部形状约束。
    float bendStiffness = 0.3f;
    // TressFX Integration 的 global shape：把可移动点按 stiffness 拉向
    // InitialPos（蒙皮/静止位置），只作用于前 effectiveRange 比例的点。
    // 两者都为 0 时 pass 内分支关闭，保持既有资产外观。
    float globalShapeStiffness = 0.0f;
    float globalShapeEffectiveRange = 0.0f;
    // TressFX VelocityShockPropagation：0 关闭 VSP 混合（Host 不录制 VSP dispatch）。
    // vspAccelThresholdMax 比较二阶位置差幅值（厘米，未除 dt²），超过时强制系数 1。
    float vspCoefficient = 0.0f;
    float vspAccelThresholdMax = VL::Hair::TressfxDefaultVspAccelThresholdMaxCm;
    // TressFX 作者逐顶点 VSPStiffness 曲线：第 i 项 = 作者曲线在 i/N 处的采样值
    // （TressFXComponent.cpp:687-691），N = pointsPerStrand。空数组表示未作者，
    // VSP pass 回退到从根前缀(1)到梢部(0)的线性近似。非空时长度必须等于该
    // guide 的 pointsPerStrand，由 TressfxModelImporter 解析 .tfx 后校验。
    std::vector<float> vspStiffness;
    // TressFX 作者逐顶点 LocalShape Stiffness：与 VSP 同源，按 i/N 采样后再
    // min(value, 0.95)（TressFXComponent.cpp:706-710）。加载期已把 > 0.95 的
    // 作者值 clamp 到 0.95 并输出记录；空数组表示回退到 bendStiffness 标量。
    std::vector<float> localShapeStiffness;
    // 放置 `hairCollision.capsules` 的解析结果；空表示不做胶囊碰撞。
    // 位置/半径都是资产局部空间厘米，与 positions 和 solver 同一坐标系。
    std::vector<VL::Hair::TressfxCollider> capsules;
    // 可选放置 `hairCollision.mesh`。缺省时只有胶囊碰撞。
    std::optional<TressfxCollisionMeshConfig> collisionMesh;
    TressfxSimulationBackend backend = TressfxSimulationBackend::Cpu;
    // 可选：根点骨骼蒙皮。仅 GPU backend 可用，因为姿态来自渲染快照而不是
    // 游戏线程；CPU 参考 solver 没有可用的 SkinPose。
    std::optional<TressfxSimulationSkinning> skinning;
};

// 运行时放置输入独立于几何导入；加载线程解析文件后，克隆只共享不可变载荷。
struct TressfxPlacementResources
{
    std::shared_ptr<const VL::Hair::SdfTriangleMesh> collisionMesh;
    std::shared_ptr<const VL::Hair::TfxBoneAsset> boneAsset;
    std::string identity;
    std::vector<std::pair<std::string, std::string>> sourceInputs;
};

// Runtime SDK 10 keeps the Games 9 wind authoring model. Curves are sampled
// at twenty authored strength points in the .stsdk table.
struct SpeedTreeWindCurve
{
    std::array<float, 20> values{};
};

struct SpeedTreeWindCommonConfig
{
    float strengthResponse = 0.0f;
    float directionResponse = 0.0f;
    float gustFrequency = 0.0f;
    float gustStrengthMin = 0.0f;
    float gustStrengthMax = 0.0f;
    float gustDurationMin = 0.0f;
    float gustDurationMax = 0.0f;
    float gustRiseScalar = 0.0f;
    float gustFallScalar = 0.0f;
};

struct SpeedTreeWindBranchConfig
{
    SpeedTreeWindCurve bend;
    SpeedTreeWindCurve oscillation;
    SpeedTreeWindCurve speed;
    SpeedTreeWindCurve turbulence;
    SpeedTreeWindCurve flexibility;
    float independence = 0.0f;
};

struct SpeedTreeWindRippleConfig
{
    SpeedTreeWindCurve planar;
    SpeedTreeWindCurve directional;
    SpeedTreeWindCurve speed;
    SpeedTreeWindCurve flexibility;
    float shimmer = 0.0f;
    float independence = 0.0f;
};

struct SpeedTreeWindConfig
{
    SpeedTreeWindCommonConfig common;
    SpeedTreeWindBranchConfig shared;
    SpeedTreeWindBranchConfig branch1;
    SpeedTreeWindBranchConfig branch2;
    SpeedTreeWindRippleConfig ripple;
    float sharedStartHeight = 0.0f;
    float branch1StretchLimit = 0.0f;
    float branch2StretchLimit = 0.0f;
    bool doShared = false;
    bool doBranch1 = false;
    bool doBranch2 = false;
    bool doRipple = false;
    bool doShimmer = false;
};

// One imported tree species' authored wind data. The key follows the mesh
// cache identity so all sections and instances of one source asset share the
// same live CPU wind state without sharing it with other species.
struct SpeedTreeWindProfile
{
    std::string key;
    Eigen::Vector3f sourceBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceBoundsMax = Eigen::Vector3f::Zero();
    SpeedTreeWindConfig config;
};

// Extra per-vertex SpeedTree attributes parsed from .stsdk. The raw source bytes
// remain available for probes while the normalized branch streams are uploaded
// as Vulkan vertex attributes for the runtime wind shader. Render tangent/sign
// is generated through MikkTSpace.
struct SpeedTreeVertexAux
{
    // Current limitation: the renderer uses only the highest-detail 3D block,
    // so LOD position and blend data remain import-side metadata. A future
    // foliage vertex stream must apply geometry morph before SDK wind.
    Eigen::Vector3f lodPosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f renderBinormal = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceNormal = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceTangent = Eigen::Vector3f::Zero();
    Eigen::Vector3f sourceBinormal = Eigen::Vector3f::Zero();
    bool hasSourceNormalVector = false;
    bool hasSourceTangentVector = false;
    bool hasSourceBinormalVector = false;
    uint8_t sourcePackedNormal = 0;
    uint8_t sourcePackedBinormal = 0;
    uint8_t sourcePackedTangent = 0;
    uint8_t ambientOcclusion = 255;
    std::array<uint8_t, 4> sourcePackedTbnAo = {0, 0, 0, 255};
    std::array<float, 4> sourceNormalizedTbnAo = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<uint8_t, 4> windBranch1 = {0, 0, 0, 0};
    std::array<uint8_t, 4> windBranch2 = {0, 0, 0, 0};
    std::array<float, 4> windBranch1Normalized = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> windBranch2Normalized = {0.0f, 0.0f, 0.0f, 0.0f};
    Eigen::Vector3f windBranch1Direction = Eigen::Vector3f::Zero();
    Eigen::Vector3f windBranch2Direction = Eigen::Vector3f::Zero();
    Eigen::Vector3f windBranch1NoiseOffsetNormalized = Eigen::Vector3f::Zero();
    Eigen::Vector3f windBranch2NoiseOffsetNormalized = Eigen::Vector3f::Zero();
};

// One skeletal clip declaration. The FBX remains the geometry/TRS source; the
// optional sequence sidecar owns runtime curves/notifies. Digests are filled by
// the loader after hashing the file bytes and are part of the import identity.
struct MeshAnimationClipPlan
{
    std::string id;
    std::string path;
    std::string sequencePath;
    std::string sequenceDigest;
};

// One AnimMontage declaration referenced by SM skinning.montages[].
// The path is the resource-relative AM_*.json; digest is filled by the loader
// after hashing the file bytes and is part of the import identity.
struct MeshAnimationMontagePlan
{
    std::string id;
    std::string path;
    std::string digest;
};

// Validated mesh asset data needed before importing the source model file.
// RendererMeshLoader uses this plan to select an importer and later bind section
// material slots.
// This structure does not own imported geometry or Vulkan resources.
struct MeshAssetBuildPlan
{
    std::string assetType;
    std::string meshAssetPath;
    std::string modelDataPath;
    MeshImportOptions importOptions;
    std::vector<MeshMaterialSlot> materialSlots;
    std::string modelCacheKey;
    // tressfx 描述符声明局部空间半径；静态 ribbon 不把逆质量当作宽度。
    float strandRadius = 0.0f;
    // TF simulationDefaults；不含皮肤绑定/碰撞，不能直接作为放置的最终配置。
    TressfxSimulationConfig tressfxSimulation;
    // 可选独立蒙皮声明；静态模型不构造骨架或额外顶点数据。
    // 骨骼网格必须给 skeletonPath：根骨、关节表与曲线注册表都以这份 SK 为准。
    std::string skeletonPath;
    std::string skeletonDigest;
    // 目标网格拥有渲染骨架；可选 sourceModel 只提供动作，retarget 显式转换姿态。
    std::string animationSourceModelPath;
    std::string retargetProfilePath;
    std::string retargetProfileDigest;
    std::vector<MeshAnimationClipPlan> animationClips;
    // 可选 AnimMontage 清单；为空时资源行为与引入 montage 前完全一致。
    std::vector<MeshAnimationMontagePlan> animationMontages;

    bool IsSkeletal() const
    {
        return !skeletonPath.empty();
    }
};

// Import request passed from RendererMeshLoader to the selected model importer strategy.
// It combines the validated mesh asset plan with the resolved absolute model path; importers return
// CPU-side ModelResource data and do not create scene objects, material instances, or GPU buffers.
struct ModelImportRequest
{
    MeshAssetBuildPlan buildPlan;
    std::string absoluteModelPath;
};

// One imported geometry section from a model file, preserving the section and material slot names.
// IModelImporter implementations fill this data; validation consumes the names to map each section
// to a VulkanLearn material instance path.
struct MeshSection
{
    std::vector<Vertex> vertices;
    // TFX positions carry inverse mass in w; retain it beside the expanded
    // ribbon vertices so the runtime solver can preserve authored root pins.
    std::vector<float> tressfxInverseMass;
    // 每根 guide 的根点（model 空间厘米）。GPU 位置 buffer 是 device-local，
    // 加载期从绑定姿态身体派生根点权重时需要一份主机侧可读副本；只留根点而不是
    // 全部点，正式资产 40,334 根约 0.48 MB。
    std::vector<Eigen::Vector3f> tressfxRootPositions;
    std::vector<uint32_t> indices;
    std::vector<SpeedTreeVertexAux> speedTreeAuxVertices;
    std::string sectionName;
    std::string materialSlotName;
    // 相机朝向 ribbon 的中心线顶点尚未展开；包围盒必须额外覆盖局部半径。
    float boundsPadding = 0.0f;
    bool isTressfx = false;
    uint32_t tressfxPointsPerStrand = 0;
};

// Runtime loading plan for one imported mesh section after material slot validation.
// RendererMeshLoader consumes this to create the section renderable and load
// the referenced material instance. It does not contain vertex or index buffers.
struct MeshSectionLoadPlan
{
    std::string sectionName;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    std::string materialInstancePath;
};

// Full model import result preserving all geometry sections from the source model.
// Importer strategies return this object; mesh validation reads section and material slot names.
struct ModelResource
{
    std::shared_ptr<const VL::Animation::SkeletalAsset> skeletalAsset;
    std::vector<MeshSection> sections;
    std::vector<std::string> sourceMaterialSlotNames;
    bool hasSpeedTreeWind = false;
    Eigen::Vector3f speedTreeSourceBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f speedTreeSourceBoundsMax = Eigen::Vector3f::Zero();
    SpeedTreeWindConfig speedTreeWind;
};
