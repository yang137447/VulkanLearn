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
