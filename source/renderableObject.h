#pragma once
#include <array>
#include <memory>
#include <vector>
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include "hair/tressfxDynamics.h"
#include "hair/tressfxLiveParams.h"
#include "hair/tressfxProxySkinning.h"
#include "hair/tressfxSimulationParams.h"
#include "hair/tressfxSkinning.h"
#include "hair/tressfxGroupPaint.h"
#include "hair/tressfxGroupBinding.h"
#include "mesh/meshAssetTypes.h"

struct MeshAssetImportResult;
struct Vertex;
namespace VL
{
class RendererBackendVulkan;
}

class RenderableObject
{
public:
    RenderableObject(
        std::vector<struct Vertex> vertices,
        std::vector<uint32_t> indices,
        VL::RendererBackendVulkan& rendererBackend,
        const std::string& name = "");
    RenderableObject(
        std::shared_ptr<const MeshAssetImportResult> imported,
        size_t sectionIndex,
        VL::RendererBackendVulkan& rendererBackend,
        const std::string& name = "",
        const TressfxSimulationConfig* tressfxConfig = nullptr,
        std::shared_ptr<const TressfxPlacementResources> tressfxResources = {},
        const std::vector<TressfxSimulationConfig>* tressfxGroupConfigs = nullptr,
        std::shared_ptr<const VL::Hair::TressfxGroupBinding> tressfxGroupBinding = {});
    ~RenderableObject();

    void Draw(vk::CommandBuffer& commandBuffer);
    const Eigen::Vector3f& GetBoundsMin() const { return boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const { return boundsMax; }
    const std::string& GetName() const { return name; }
    // GPU 预蒙皮按 gl_VertexIndex 直接寻址：compute 把顶点流当 SSBO 读，
    // 顶点数量决定输出 buffer 大小与 dispatch group 数。
    uint32_t GetVertexCount() const { return static_cast<uint32_t>(GetVertices().size()); }
    const vk::DescriptorBufferInfo& GetVertexBufferInfo() const { return vertexBufferInfo; }
    const vk::DescriptorBufferInfo& GetSkinInfluenceInfo() const { return skinInfluenceInfo; }
    bool IsTressfx() const;
    // 放置绑定解析器在 spawn 时复用不可变几何根点和来源骨骼资产；行为另由放置携带。
    const std::shared_ptr<const MeshAssetImportResult>& GetImported() const { return imported; }
    size_t GetImportedSectionIndex() const { return importedSectionIndex; }
    const vk::DescriptorBufferInfo& GetTressfxPositionInfo() const { return tressfxPositionInfo; }
    // 每 guide 一个 group id（binding 5）与 vec4 组颜色表（binding 6）。
    // descriptor 捕获固定容量 buffer；草稿组同步只原地重写内容，UI 不直接写 GPU。
    const vk::DescriptorBufferInfo& GetTressfxGuideGroupIdInfo() const
    {
        return tressfxGuideGroupIdInfo;
    }
    const vk::DescriptorBufferInfo& GetTressfxGuideGroupColorInfo() const
    {
        return tressfxGuideGroupColorInfo;
    }
    // compute Set 0 binding 17：每组 8 个 float 的解析后配置；按 frame-in-flight
    // 分槽，因为实时参数覆盖会改写它。索引 0 = base + placement。
    const vk::DescriptorBufferInfo& GetTressfxGuideGroupConfigInfo(
        uint32_t frameSlot) const
    {
        return tressfxGuideGroupConfigTargets[frameSlot].info;
    }
    void WriteTressfxGroupSimulationConfigs(uint32_t frameSlot);
    // 只有至少一个组声明了 vspCoefficient > 0 时才需要录制 VSP pass。
    bool HasTressfxGroupVsp() const;
    const std::vector<TressfxSimulationConfig>& GetTressfxGroupSimulationConfigs() const
    {
        return tressfxGroupSimulationConfigs;
    }
    // 笔刷 ghost（binding 7）：每 guide 一个 float；空列表即清除。
    const vk::DescriptorBufferInfo& GetTressfxBrushWeightInfo(uint32_t frameSlot) const
    {
        return tressfxBrushWeightInfos.at(
            frameSlot < tressfxBrushWeightInfos.size() ? frameSlot : 0);
    }
    bool HasTressfxBrushPreview() const { return tressfxBrushActive; }
    bool TressfxBrushRemoveMode() const
    {
        return tressfxBrushMode == VL::Hair::GroupPaintMode::Remove;
    }
    // 只由渲染线程调用；weights 是整笔的完整 guide->weight 映射，不是增量。
    bool SetTressfxBrushPreview(uint32_t targetGroupId, VL::Hair::GroupPaintMode mode,
        const std::vector<VL::Hair::TressfxBrushGuideWeight>& weights,
        uint32_t frameSlot, std::string& error);
    // 颜色表项数 = 1（默认组）+ 命名组数；shader 用 group id 直接索引。
    uint32_t GetTressfxGuideGroupCount() const { return tressfxGuideGroupColorCount; }
    const std::string& GetTressfxGroupBindingKey() const
    {
        return tressfxGroupBindingKey;
    }
    // 组 overrides 的身份：与 binding 分开比较，避免每次 World 重载都重写
    // group 参数 buffer。
    const std::string& GetTressfxGroupSimulationKey() const
    {
        return tressfxGroupSimulationKey;
    }
    // 组编辑后的原地更新：只重写 per-object group id/颜色/组参数 buffer 与
    // CPU solver 的 guide->group 映射，不重建几何、不换 RenderableObject。
    // 颜色表与参数表按 TressfxGroupPreviewColorCapacity 预留，支持组数增删；
    // 超过容量返回错误且不修改任何已有数据。
    bool UpdateTressfxGroupBinding(
        const VL::Hair::TressfxGroupBinding& binding,
        const std::vector<TressfxSimulationConfig>& groupConfigs,
        std::string& error);
    // 草稿预览入口：没有保存后的 per-group 解析结果时，保留旧名称对应的配置；
    // 新建/重命名组回退到 index 0（base + placement），保存重载后再应用 override。
    bool UpdateTressfxGroupBinding(
        const VL::Hair::TressfxGroupBinding& binding,
        std::string& error);
    // 游戏线程推进 CPU 参考 solver。sceneWindVelocityCmPerSec 是场景风向量
    // （direction * strength，厘米/秒），在 StepFrame 之前写入 solver config；
    // CPU 参考 backend 不消费场景阵风（GT 没有渲染线程的阵风时钟）。
    void AdvanceTressfxSimulation(float dt, const Eigen::Vector3f& sceneWindVelocityCmPerSec);

    // GPU backend 的模拟资源合同。current positions 始终与 graphics Object Set 2
    // binding 3 共用同一 buffer；rest/params 也服务 off 蒙皮，previous 仅供模拟。
    bool UsesGpuGuideSimulation() const;
    bool UsesGpuHairSkinning() const;
    const std::shared_ptr<const TressfxPlacementResources>& GetTressfxResources() const
    { return tressfxPlacementResources; }
    const TressfxSimulationConfig& GetTressfxSimulationConfig() const;
    // 编辑器/Console 的实时覆盖：只改本对象自己的 config 副本，下一帧组 params UBO
    // 时生效；不重建 buffer、不改模型缓存身份。仅渲染线程、仅 GPU backend。
    bool ApplyTressfxLiveParameter(const VL::Hair::HairLiveParameterRequest& request,
        std::string& error);
    // World 构建阶段解析出的根点骨骼绑定。未声明放置 skinning 时 enabled=false，
    // 运行时保持历史 rest pin；解析失败不会走到这里，加载期已硬错误。
    const VL::Hair::TressfxSkinningBinding& GetTressfxSkinningBinding() const
    {
        return tressfxSkinningBinding;
    }
    void SetTressfxSkinningBinding(const VL::Hair::TressfxSkinningBinding& binding);
    // 运行时 spawn 的 per-placement 网格：共享模板的导入几何（imported/section），
    // 新建自己的 GPU 资源与可变状态。具体“放置绑定”由
    // placementBindingResolver.h 按 PlacementBinding::Kind 分派，本方法不感知类型。
    // 仅在渲染线程、resolve 之前调用；失败抛 std::runtime_error。
    std::shared_ptr<RenderableObject> CloneForPlacement(const std::string& newName,
        const TressfxSimulationConfig* effectiveConfig = nullptr,
        const std::vector<TressfxSimulationConfig>* groupConfigs = nullptr) const;
    // 多骨 GPU 资源：逐 strand 4×{joint,weight} 与每 frame-in-flight 的 palette。
    // 未声明 .tfxbone 时权重为 0，shader 用 motion.w bit2 判断是否读取。
    const vk::DescriptorBufferInfo& GetTressfxBoneWeightsInfo() const
    {
        return tressfxBoneWeightsInfo;
    }
    const vk::DescriptorBufferInfo& GetTressfxSkinningPaletteInfo(uint32_t frameSlot) const
    {
        return tressfxPaletteTargets[frameSlot].info;
    }
    // 每帧碰撞前的最大穿透深度（x=胶囊、y=SDF，厘米；0 表示无穿透）。
    // 由 collision/SDF collide shader atomicMax，Host 每帧清零。
    const vk::DescriptorBufferInfo& GetTressfxPenetrationStatsInfo(uint32_t frameSlot) const
    {
        return tressfxPenetrationStatsTargets[frameSlot].info;
    }
    void ResetTressfxPenetrationStats(uint32_t frameSlot);
    void WriteTressfxSkinningPalette(
        const std::vector<Eigen::Matrix4f>& jointMatrices, uint32_t frameSlot);
    uint32_t GetTressfxGuideCount() const { return tressfxGuideCount; }
    uint32_t GetTressfxPointsPerGuide() const { return tressfxPointsPerGuide; }
    const vk::DescriptorBufferInfo& GetTressfxPreviousPositionInfo() const
    {
        return tressfxPreviousPositionInfo;
    }
    // previous-previous：TressFX 的伪加速度和第二阶 Verlet 状态。
    const vk::DescriptorBufferInfo& GetTressfxPreviousPreviousPositionInfo() const
    {
        return tressfxPreviousPreviousPositionInfo;
    }
    const vk::DescriptorBufferInfo& GetTressfxRestPositionInfo() const
    {
        return tressfxRestPositionInfo;
    }
    // 每个点一条静止段长（i -> i+1，末项 0），供 LengthConstraintsWind pass 读取。
    const vk::DescriptorBufferInfo& GetTressfxRestLengthInfo() const
    {
        return tressfxRestLengthInfo;
    }
    // 每根 strand 3 个 vec4：VspQuat、VspTranslation.xyz + VspCoeff.w、SkinningQuat。
    const vk::DescriptorBufferInfo& GetTressfxStrandLevelInfo() const
    {
        return tressfxStrandLevelInfo;
    }
    // 解析胶囊 SSBO；作者胶囊每帧可能随骨骼移动，因此按 frame-in-flight 分槽，
    // 避免主机写当前槽时覆盖仍在飞的上一边界。无 bone 的胶囊内容保持不变。
    const vk::DescriptorBufferInfo& GetTressfxColliderInfo(uint32_t frameSlot) const
    {
        return tressfxColliderTargets[frameSlot].info;
    }
    // 上传本帧胶囊（a/b 已按各自关节的 bind→pose 矩阵变换，radius 不缩放）。
    void WriteTressfxColliders(const std::vector<VL::Hair::TressfxCollider>& capsules,
        uint32_t frameSlot);
    // 逐帧碰撞距离场（`collision.mesh`）存在时创建；未声明时这些 buffer 与
    // info 都为空，SDF 的三个 pass 不会被录制。SDF params 按 frame-in-flight
    // 分槽，因为域（origin/cellSize）每帧随蒙皮后的代理包围盒更新。
    bool HasTressfxSdfCollision() const { return tressfxSdfCollision; }
    const vk::DescriptorBufferInfo& GetTressfxSdfParamsInfo(uint32_t frameSlot) const
    {
        return tressfxSdfParamsTargets[frameSlot].info;
    }
    const vk::DescriptorBufferInfo& GetTressfxSdfGridInfo() const
    {
        return tressfxSdfGridInfo;
    }
    const vk::DescriptorBufferInfo& GetTressfxSdfTriangleInfo() const
    {
        return tressfxSdfTriangleInfo;
    }
    const vk::DescriptorBufferInfo& GetTressfxSdfIndexInfo() const
    {
        return tressfxSdfIndexInfo;
    }
    uint32_t GetTressfxSdfTriangleCount() const { return tressfxSdfTriangleCount; }
    uint32_t GetTressfxSdfCellCount() const { return tressfxSdfCellCount; }
    // 代理逐顶点权重转移（加载期从绑定姿态身体最近顶点复制）。为空表示没有
    // 可用权重；SDF splat 仍走单骨根矩阵回退，行为与引入转移前一致。
    bool HasTressfxSdfProxySkinning() const { return tressfxSdfProxySkin != nullptr; }
    // 放置绑定解析器复制模板状态时读取；没有声明 collision.mesh 时为空。
    const std::shared_ptr<const VL::Hair::SdfProxySkinTransfer>& GetTressfxSdfProxySkin() const
    {
        return tressfxSdfProxySkin;
    }
    const vk::DescriptorBufferInfo& GetTressfxSdfProxyInfluenceInfo() const
    {
        return tressfxSdfProxyInfluenceInfo;
    }
    // SDF splat 的 palette 槽：有转移时是全骨架 bind→pose（每关节 current +
    // previous 两份矩阵，与图形 palette 同布局）；没有转移时是 1 关节占位。
    // shader 的 counts.y=0 回退路径不读它，但 descriptor 必须始终有效。
    const vk::DescriptorBufferInfo& GetTressfxSdfProxyPaletteInfo(uint32_t frameSlot) const
    {
        return tressfxSdfProxyPaletteTargets[frameSlot].info;
    }
    // 每帧把全骨架 bind→pose 矩阵写进当前 frame-in-flight 槽。布局与
    // engine/skinnedVertexMath.glsl 的 SkinJoint 相同：current 后跟 previous，
    // 这里两者填同一矩阵（splat 只消费 current）。
    void WriteTressfxSdfProxyPalette(
        const std::vector<Eigen::Matrix4f>& bindToPoseMatrices, uint32_t frameSlot);
    // 加载期写入转移表并创建 device-local 权重 SSBO。同一 mesh key 的多个场景
    // placement 共享本对象，重复调用保留第一份转移（同一资产权重相同）；
    // descriptor set 尚未创建时这是纯资源准备，不触碰在飞帧。
    void SetTressfxSdfProxySkin(
        std::shared_ptr<const VL::Hair::SdfProxySkinTransfer> transfer);
    // frameSlot 是 frame-in-flight 槽，与 params UBO 的槽一一对应。
    const vk::DescriptorBufferInfo& GetTressfxParamsInfo(uint32_t frameSlot) const;
    void WriteTressfxSimulationParams(
        const VL::Hair::GuideSimulationParams& params,
        uint32_t frameSlot);
    // reset/首轮 Integration 专用参数块。源 reset 的首轮 dispatch 与同帧后续
    // substep 的正常步进不能共用一个 UBO（同一命令缓冲执行时只看最后一次写入），
    // 因此单独分配一份；正常 params[slot] 永远保持 motion.y = 0。
    const vk::DescriptorBufferInfo& GetTressfxResetParamsInfo(
        uint32_t frameSlot) const;
    void WriteTressfxResetParams(
        const VL::Hair::GuideSimulationParams& params,
        uint32_t frameSlot);
    // 每帧从蒙皮后的代理包围盒重算立方体域，写当前 frame slot 的 SDF params。
    // jointMatrices 非空且对象有逐顶点权重时，域由同一套 LBS 权重计算；否则
    // 回退到 skinningMatrix。非有限包围盒返回 false（pass 跳过该帧 SDF dispatch
    // 并只报告一次）。
    bool WriteTressfxSdfParams(const Eigen::Matrix4f& skinningMatrix,
        const std::vector<Eigen::Matrix4f>* jointMatrices, uint32_t frameSlot);

private:
    void UpdateLocalBounds();
    const std::vector<Vertex>& GetVertices() const;
    const std::vector<uint32_t>& GetIndices() const;

    void CreateVertexBuffer();
    void DestroyVertexBuffer();

    void CreateIndexBuffer();
    void DestroyIndexBuffer();
    void CreateSkinInfluenceBuffer();
    void CreateTressfxPositionBuffer();
    void CreateTressfxGuideGroupBuffers();
    void CreateTressfxBrushWeightBuffer();
    void CreateTressfxGpuSimulationBuffers();
    void CreateTressfxSdfBuffers();
    void DestroyTressfxBuffers();

    // 每个 frame-in-flight 槽一份 params UBO，避免主机写当前槽时覆盖仍在飞的上一帧参数。
    struct TressfxParamsTarget
    {
        vk::Buffer buffer = nullptr;
        vk::DeviceMemory memory = nullptr;
        void* mapped = nullptr;
        vk::DescriptorBufferInfo info;
    };

    std::string name;
    VL::RendererBackendVulkan* rendererBackend = nullptr;

    std::vector<struct Vertex> vertices;
    std::vector<uint32_t> indices;
    // World 导入后和离线候选包共享 section；上传不再复制大块 CPU 顶点。
    std::shared_ptr<const MeshAssetImportResult> imported;
    size_t importedSectionIndex = 0;
    Eigen::Vector3f boundsMin;
    Eigen::Vector3f boundsMax;
    vk::Buffer vertexBuffer = nullptr;
    vk::DeviceMemory vertexBufferMemory = nullptr;
    vk::DescriptorBufferInfo vertexBufferInfo;
    vk::Buffer indexBuffer = nullptr;
    vk::DeviceMemory indexBufferMemory = nullptr;
    vk::DescriptorBufferInfo indexBufferInfo;
    vk::Buffer skinInfluenceBuffer = nullptr;
    vk::DeviceMemory skinInfluenceMemory = nullptr;
    vk::DescriptorBufferInfo skinInfluenceInfo;
    vk::Buffer tressfxPositionBuffer = nullptr;
    vk::DeviceMemory tressfxPositionMemory = nullptr;
    vk::DescriptorBufferInfo tressfxPositionInfo;
    vk::Buffer tressfxGuideGroupIdBuffer = nullptr;
    vk::DeviceMemory tressfxGuideGroupIdMemory = nullptr;
    vk::DescriptorBufferInfo tressfxGuideGroupIdInfo;
    vk::Buffer tressfxGuideGroupColorBuffer = nullptr;
    vk::DeviceMemory tressfxGuideGroupColorMemory = nullptr;
    vk::DescriptorBufferInfo tressfxGuideGroupColorInfo;
    uint32_t tressfxGuideGroupColorCount = 0;
    // 每组解析配置按 frame-in-flight 分槽；内容通常静态，但实时参数覆盖会在
    // 当前槽 fence 完成后重写，避免与仍在执行的 compute 读同一 buffer。
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxGuideGroupConfigTargets;
    // per-swapchain ghost 权重：上一帧仍可能读取同一槽，不能每帧覆写单 buffer。
    std::vector<vk::Buffer> tressfxBrushWeightBuffers;
    std::vector<vk::DeviceMemory> tressfxBrushWeightMemories;
    std::vector<vk::DescriptorBufferInfo> tressfxBrushWeightInfos;
    bool tressfxBrushActive = false;
    VL::Hair::GroupPaintMode tressfxBrushMode = VL::Hair::GroupPaintMode::Add;
    vk::Buffer tressfxPreviousPositionBuffer = nullptr;
    vk::DeviceMemory tressfxPreviousPositionMemory = nullptr;
    vk::DescriptorBufferInfo tressfxPreviousPositionInfo;
    vk::Buffer tressfxPreviousPreviousPositionBuffer = nullptr;
    vk::DeviceMemory tressfxPreviousPreviousPositionMemory = nullptr;
    vk::DescriptorBufferInfo tressfxPreviousPreviousPositionInfo;
    vk::Buffer tressfxRestPositionBuffer = nullptr;
    vk::DeviceMemory tressfxRestPositionMemory = nullptr;
    vk::DescriptorBufferInfo tressfxRestPositionInfo;
    vk::Buffer tressfxRestLengthBuffer = nullptr;
    vk::DeviceMemory tressfxRestLengthMemory = nullptr;
    vk::DescriptorBufferInfo tressfxRestLengthInfo;
    vk::Buffer tressfxStrandLevelBuffer = nullptr;
    vk::DeviceMemory tressfxStrandLevelMemory = nullptr;
    vk::DescriptorBufferInfo tressfxStrandLevelInfo;
    vk::Buffer tressfxColliderBuffer = nullptr;
    vk::DeviceMemory tressfxColliderMemory = nullptr;
    vk::DescriptorBufferInfo tressfxColliderInfo;
    // 胶囊 SSBO 按 frame-in-flight 分槽：绑定骨骼后内容逐帧变化，主机只写
    // 已完成 fence 的当前槽（与 params UBO 同一同步策略）。
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxColliderTargets;
    // 逐帧碰撞距离场：device-local 的固定分辨率 uint 网格 + 静态代理顶点/索引
    // + 每个 frame-in-flight 槽一份 64-byte std140 参数块（域每帧变化）。
    vk::Buffer tressfxSdfGridBuffer = nullptr;
    vk::DeviceMemory tressfxSdfGridMemory = nullptr;
    vk::DescriptorBufferInfo tressfxSdfGridInfo;
    vk::Buffer tressfxSdfTriangleBuffer = nullptr;
    vk::DeviceMemory tressfxSdfTriangleMemory = nullptr;
    vk::DescriptorBufferInfo tressfxSdfTriangleInfo;
    vk::Buffer tressfxSdfIndexBuffer = nullptr;
    vk::DeviceMemory tressfxSdfIndexMemory = nullptr;
    vk::DescriptorBufferInfo tressfxSdfIndexInfo;
    // `collision.mesh` 的逐顶点权重（加载期从绑定姿态身体转移一次）与两个
    // 常驻 SSBO：代理影响流（CSR，供 splat 每三角形 LBS）和 1 关节的 palette
    // 占位。真实 palette 由渲染线程按 swapchain image 槽临时绑定。
    std::shared_ptr<const VL::Hair::SdfProxySkinTransfer> tressfxSdfProxySkin;
    vk::Buffer tressfxSdfProxyInfluenceBuffer = nullptr;
    vk::DeviceMemory tressfxSdfProxyInfluenceMemory = nullptr;
    vk::DescriptorBufferInfo tressfxSdfProxyInfluenceInfo;
    // 每 frame-in-flight 槽一份 host-coherent palette（每关节 128 bytes）。
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxSdfProxyPaletteTargets;
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxSdfParamsTargets;
    uint32_t tressfxSdfTriangleCount = 0;
    uint32_t tressfxSdfCellCount = 0;
    uint32_t tressfxSdfResolution = 0;
    bool tressfxSdfCollision = false;
    bool reportedSdfDomain = false;
    bool reportedSdfDomainFailure = false;
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxParamsTargets;
    // reset 首轮 / placement 换基的首个 Integration 专用；与正常 params 同布局、
    // 同槽数，只在需要时写入。
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxResetParamsTargets;
    // 多骨：逐 strand 4×(joint,weight) 静态权重 + 每槽全骨架 palette。
    vk::Buffer tressfxBoneWeightsBuffer = nullptr;
    vk::DeviceMemory tressfxBoneWeightsMemory = nullptr;
    vk::DescriptorBufferInfo tressfxBoneWeightsInfo;
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxPaletteTargets;
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxPenetrationStatsTargets;
    uint32_t tressfxPaletteJointCapacity = 0;
    uint32_t tressfxGuideCount = 0;
    uint32_t tressfxPointsPerGuide = 0;
    bool tressfxGpuSimulation = false;
    // 本渲染对象自己的模拟参数副本：创建时从 section 复制，实时覆盖只改这里。
    // imported/section 是共享导入结果，写回会污染其他 placement。
    // CPU 参考 solver 在构造时另有一份副本，实时覆盖只服务 GPU backend。
    TressfxSimulationConfig tressfxSimulationConfig;
    // index 0 = base + placement，k = base + 第 k 组 overrides + placement。
    // CPU/GPU 都从这一份值语义数组取值；不写回共享导入结果。
    std::vector<TressfxSimulationConfig> tressfxGroupSimulationConfigs;
    // 已保存资产的 guide->group 映射与组名顺序；与导入几何分离。
    std::shared_ptr<const VL::Hair::TressfxGroupBinding> tressfxGuideGroupBinding;
    std::string tressfxGroupBindingKey;
    std::string tressfxGroupSimulationKey;
    std::shared_ptr<const TressfxPlacementResources> tressfxPlacementResources;
    VL::Hair::TressfxSkinningBinding tressfxSkinningBinding;
    std::unique_ptr<VL::Hair::GuideSimulation> tressfxSimulation;
};
