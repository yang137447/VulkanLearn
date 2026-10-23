#pragma once
#include <array>
#include <memory>
#include <vector>
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include "hair/tressfxDynamics.h"
#include "hair/tressfxSimulationParams.h"
#include "hair/tressfxSkinning.h"
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
        const std::string& name = "");
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
    const vk::DescriptorBufferInfo& GetTressfxPositionInfo() const { return tressfxPositionInfo; }
    void AdvanceTressfxSimulation(float dt);

    // GPU backend 的模拟资源合同。current positions 始终与 graphics Object Set 2
    // binding 3 共用同一 buffer；previous/rest/params 只在 GPU backend 下分配。
    bool UsesGpuGuideSimulation() const;
    const TressfxSimulationConfig& GetTressfxSimulationConfig() const;
    // World 构建阶段解析出的根点骨骼绑定。未声明 simulation.skinning 时 enabled=false，
    // 运行时保持历史 rest pin；解析失败不会走到这里，加载期已硬错误。
    const VL::Hair::TressfxSkinningBinding& GetTressfxSkinningBinding() const
    {
        return tressfxSkinningBinding;
    }
    void SetTressfxSkinningBinding(const VL::Hair::TressfxSkinningBinding& binding)
    {
        tressfxSkinningBinding = binding;
    }
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
    // 静态解析胶囊 SSBO；创建时上传一次，GPU kernel 按 colliderCount 读取。
    const vk::DescriptorBufferInfo& GetTressfxColliderInfo() const
    {
        return tressfxColliderInfo;
    }
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
    // frameSlot 是 frame-in-flight 槽，与 params UBO 的槽一一对应。
    const vk::DescriptorBufferInfo& GetTressfxParamsInfo(uint32_t frameSlot) const;
    void WriteTressfxSimulationParams(
        const VL::Hair::GuideSimulationParams& params,
        uint32_t frameSlot);
    // 每帧从蒙皮后的代理包围盒重算立方体域，写当前 frame slot 的 SDF params。
    // 非有限包围盒返回 false（pass 会跳过该帧的三个 SDF dispatch 并只报告一次）。
    bool WriteTressfxSdfParams(const Eigen::Matrix4f& skinningMatrix, uint32_t frameSlot);

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
    uint32_t tressfxGuideCount = 0;
    uint32_t tressfxPointsPerGuide = 0;
    bool tressfxGpuSimulation = false;
    VL::Hair::TressfxSkinningBinding tressfxSkinningBinding;
    std::unique_ptr<VL::Hair::GuideSimulation> tressfxSimulation;
};
