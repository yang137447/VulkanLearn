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
    // frameSlot 是 frame-in-flight 槽，与 params UBO 的槽一一对应。
    const vk::DescriptorBufferInfo& GetTressfxParamsInfo(uint32_t frameSlot) const;
    void WriteTressfxSimulationParams(
        const VL::Hair::GuideSimulationParams& params,
        uint32_t frameSlot);

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
    std::array<TressfxParamsTarget, VL::Hair::GuideSimulationParamBufferCount>
        tressfxParamsTargets;
    uint32_t tressfxGuideCount = 0;
    uint32_t tressfxPointsPerGuide = 0;
    bool tressfxGpuSimulation = false;
    VL::Hair::TressfxSkinningBinding tressfxSkinningBinding;
    std::unique_ptr<VL::Hair::GuideSimulation> tressfxSimulation;
};
