#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>

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
        const std::string& name = "",
        bool dynamicVertices = false);
    RenderableObject(
        std::shared_ptr<const MeshAssetImportResult> imported,
        size_t sectionIndex,
        VL::RendererBackendVulkan& rendererBackend,
        const std::string& name = "");
    // 固定容量动态网格：顶点/索引缓冲按 capacity 一次分配，内容可整批替换。
    // 碎片池用它避免每片创建/销毁 buffer；容量不足时调用方应换更大的对象。
    RenderableObject(
        size_t vertexCapacity,
        size_t indexCapacity,
        VL::RendererBackendVulkan& rendererBackend,
        const std::string& name = "");
    ~RenderableObject();

    void Draw(vk::CommandBuffer& commandBuffer);
    // 固定拓扑软体每帧只更新顶点位置/法线；动态模式使用 host-visible 顶点缓冲。
    // 调用方必须保持顶点数量不变，拓扑变化时创建新的 RenderableObject。
    void UpdateVertices(const std::vector<struct Vertex>& vertices);
    // 固定容量对象的整批替换；拓扑可以变化，但不得超过容量。
    void UpdateDynamicMesh(const std::vector<struct Vertex>& vertices,
        const std::vector<uint32_t>& indices);
    size_t GetVertexCapacity() const { return vertexCapacity; }
    size_t GetIndexCapacity() const { return indexCapacity; }
    const Eigen::Vector3f& GetBoundsMin() const { return boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const { return boundsMax; }
    const std::string& GetName() const { return name; }
    const vk::DescriptorBufferInfo& GetSkinInfluenceInfo() const { return skinInfluenceInfo; }

private:
    void UpdateLocalBounds();
    const std::vector<Vertex>& GetVertices() const;
    const std::vector<uint32_t>& GetIndices() const;

    void CreateVertexBuffer();
    void DestroyVertexBuffer();

    void CreateIndexBuffer();
    void DestroyIndexBuffer();
    void CreateSkinInfluenceBuffer();

    std::string name;
    VL::RendererBackendVulkan* rendererBackend = nullptr;

    std::vector<struct Vertex> vertices;
    std::vector<uint32_t> indices;
    bool dynamicVertices = false;
    bool fixedCapacity = false;
    size_t vertexCapacity = 0;
    size_t indexCapacity = 0;
    uint32_t drawIndexCount = 0;
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
};
