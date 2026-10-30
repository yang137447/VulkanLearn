#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "render/backend/rendererBackendVulkan.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "animation/skeletalPlayback.h"

#include <cstring>
#include <stdexcept>
#include <tuple>

RenderableObject::RenderableObject(
    std::vector<Vertex> vertices,
    std::vector<uint32_t> indices,
    VL::RendererBackendVulkan& rendererBackend,
    const std::string& name,
    bool dynamicVertices)
{
    this->rendererBackend = &rendererBackend;
    this->vertices = std::move(vertices);
    this->indices = std::move(indices);
    this->dynamicVertices = dynamicVertices;
    this->name = name;

    //更新boundingBox
    UpdateLocalBounds();
    // 创建顶点缓冲区
    CreateVertexBuffer();
    // 创建索引缓冲区
    CreateIndexBuffer();
}
RenderableObject::RenderableObject(
    std::shared_ptr<const MeshAssetImportResult> importedResource,
    size_t sectionIndex,
    VL::RendererBackendVulkan& backend,
    const std::string& objectName)
    : imported(std::move(importedResource))
    , importedSectionIndex(sectionIndex)
{
    if (!imported || sectionIndex >= imported->modelResource.sections.size())
        throw std::invalid_argument("Invalid imported mesh section");
    rendererBackend = &backend;
    name = objectName;
    UpdateLocalBounds();
    CreateVertexBuffer();
    CreateIndexBuffer();
    try
    {
        CreateSkinInfluenceBuffer();
    }
    catch (...)
    {
        DestroyIndexBuffer();
        DestroyVertexBuffer();
        throw;
    }
}

RenderableObject::RenderableObject(
    size_t vertexCapacity,
    size_t indexCapacity,
    VL::RendererBackendVulkan& backend,
    const std::string& objectName)
    : fixedCapacity(true)
    , vertexCapacity(vertexCapacity)
    , indexCapacity(indexCapacity)
{
    if (vertexCapacity == 0 || indexCapacity == 0)
        throw std::invalid_argument("Dynamic mesh capacity must be positive");
    rendererBackend = &backend;
    name = objectName;
    dynamicVertices = true;
    CreateVertexBuffer();
    CreateIndexBuffer();
}

const std::vector<Vertex>& RenderableObject::GetVertices() const
{
    return imported ? imported->modelResource.sections[importedSectionIndex].vertices
                    : vertices;
}

const std::vector<uint32_t>& RenderableObject::GetIndices() const
{
    return imported ? imported->modelResource.sections[importedSectionIndex].indices
                    : indices;
}

RenderableObject::~RenderableObject()
{
    if (rendererBackend && skinInfluenceBuffer)
        rendererBackend->DestroyBuffer(skinInfluenceBuffer, skinInfluenceMemory);
    // 销毁索引缓冲区
    DestroyIndexBuffer();
    // 销毁顶点缓冲区
    DestroyVertexBuffer();
    rendererBackend = nullptr;
}

void RenderableObject::CreateSkinInfluenceBuffer()
{
    if (!imported || !imported->modelResource.skeletalAsset) return;
    const auto words = VL::Animation::PackSkinInfluences(
        imported->modelResource.skeletalAsset->mesh.sections.at(importedSectionIndex));
    const vk::DeviceSize size = words.size() * sizeof(uint32_t);
    std::tie(skinInfluenceBuffer, skinInfluenceMemory) = rendererBackend->CreateBuffer(
        size, vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        "SkinInfluences: " + name);
    try
    {
        void* mapped = rendererBackend->MapMemory(skinInfluenceMemory, size);
        std::memcpy(mapped, words.data(), size);
        rendererBackend->UnmapMemory(skinInfluenceMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(skinInfluenceBuffer, skinInfluenceMemory);
        skinInfluenceBuffer = nullptr;
        skinInfluenceMemory = nullptr;
        throw;
    }
    skinInfluenceInfo.setBuffer(skinInfluenceBuffer).setOffset(0).setRange(size);
}

void RenderableObject::Draw(vk::CommandBuffer &commandBuffer)
{
    // viewport/scissor 由 PassRuntime 按 pass 统一设置，避免每个对象把中心
    // 视口覆盖回整张资源。
    commandBuffer.bindVertexBuffers(0, 1, &vertexBuffer, &vertexBufferInfo.offset);
    commandBuffer.bindIndexBuffer(indexBuffer, 0, vk::IndexType::eUint32);
    const size_t count = fixedCapacity ? drawIndexCount : GetIndices().size();
    if (count == 0) return;
    commandBuffer.drawIndexed(static_cast<uint32_t>(count), 1, 0, 0, 0);
}

void RenderableObject::CreateVertexBuffer()
{
    if (fixedCapacity)
    {
        const vk::DeviceSize bufferSize = vertexCapacity * sizeof(Vertex);
        std::tie(vertexBuffer, vertexBufferMemory) = rendererBackend->CreateBuffer(
            bufferSize,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "PooledVertexBuffer (" + name + ")");
        vertexBufferInfo.setBuffer(vertexBuffer).setOffset(0).setRange(bufferSize);
        return;
    }
    const auto& sourceVertices = GetVertices();
    vk::DeviceSize bufferSize = sourceVertices.size() * sizeof(Vertex);
    if (dynamicVertices)
    {
        // 软体碎片拓扑固定，顶点数量不会变化；用 host-visible 缓冲避免每帧
        // 创建 staging buffer 和一次额外的 device-local 拷贝。
        std::tie(vertexBuffer, vertexBufferMemory) = rendererBackend->CreateBuffer(
            bufferSize,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "DynamicVertexBuffer (" + name + ")");
        void* data = rendererBackend->MapMemory(vertexBufferMemory, bufferSize);
        std::memcpy(data, sourceVertices.data(), static_cast<size_t>(bufferSize));
        rendererBackend->UnmapMemory(vertexBufferMemory);
        vertexBufferInfo
            .setBuffer(vertexBuffer)
            .setOffset(0)
            .setRange(bufferSize);
        return;
    }

    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        usage,
        memoryPropertyFlags,
        "StagingBuffer: Vertex (" + name + ")");

    void *data = rendererBackend->MapMemory(stagingBufferMemory, bufferSize);
    std::memcpy(data, sourceVertices.data(), static_cast<size_t>(bufferSize));
    rendererBackend->UnmapMemory(stagingBufferMemory);

    vk::BufferUsageFlags vertexBufferUsage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags vertexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(vertexBuffer, vertexBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        vertexBufferUsage,
        vertexBufferMemoryPropertyFlags,
        "VertexBuffer (" + name + ")");

    rendererBackend->CopyBufferToBuffer(stagingBuffer, vertexBuffer, bufferSize);
    rendererBackend->DestroyBuffer(stagingBuffer, stagingBufferMemory);

    vertexBufferInfo
        .setBuffer(vertexBuffer)
        .setOffset(0)
        .setRange(sourceVertices.size() * sizeof(Vertex));
}

void RenderableObject::UpdateVertices(const std::vector<Vertex>& updatedVertices)
{
    if (imported)
        throw std::logic_error("Imported meshes cannot be updated in place");
    if (!dynamicVertices)
        throw std::logic_error("RenderableObject is not using dynamic vertices");
    if (updatedVertices.size() != vertices.size())
        throw std::invalid_argument(
            "Dynamic vertex updates must preserve the vertex count");
    if (updatedVertices.empty()) return;

    const vk::DeviceSize bufferSize =
        static_cast<vk::DeviceSize>(updatedVertices.size() * sizeof(Vertex));
    void* data = rendererBackend->MapMemory(vertexBufferMemory, bufferSize);
    std::memcpy(data, updatedVertices.data(), static_cast<size_t>(bufferSize));
    rendererBackend->UnmapMemory(vertexBufferMemory);
    vertices = updatedVertices;
    UpdateLocalBounds();
}

void RenderableObject::UpdateDynamicMesh(
    const std::vector<Vertex>& updatedVertices,
    const std::vector<uint32_t>& updatedIndices)
{
    if (imported) throw std::logic_error("Imported meshes cannot be updated in place");
    if (!fixedCapacity)
        throw std::logic_error("UpdateDynamicMesh requires a fixed-capacity mesh");
    if (updatedVertices.size() > vertexCapacity || updatedIndices.size() > indexCapacity)
        throw std::invalid_argument("Dynamic mesh update exceeds its capacity");
    if (!updatedVertices.empty())
    {
        const vk::DeviceSize size =
            static_cast<vk::DeviceSize>(updatedVertices.size() * sizeof(Vertex));
        void* data = rendererBackend->MapMemory(vertexBufferMemory, size);
        std::memcpy(data, updatedVertices.data(), static_cast<size_t>(size));
        rendererBackend->UnmapMemory(vertexBufferMemory);
    }
    if (!updatedIndices.empty())
    {
        const vk::DeviceSize size =
            static_cast<vk::DeviceSize>(updatedIndices.size() * sizeof(uint32_t));
        void* data = rendererBackend->MapMemory(indexBufferMemory, size);
        std::memcpy(data, updatedIndices.data(), static_cast<size_t>(size));
        rendererBackend->UnmapMemory(indexBufferMemory);
    }
    vertices = updatedVertices;
    indices = updatedIndices;
    drawIndexCount = static_cast<uint32_t>(indices.size());
    UpdateLocalBounds();
}

void RenderableObject::DestroyVertexBuffer()
{
    if (rendererBackend != nullptr)
    {
        rendererBackend->DestroyBuffer(vertexBuffer, vertexBufferMemory);
        vertexBuffer = nullptr;
        vertexBufferMemory = nullptr;
    }
}

void RenderableObject::CreateIndexBuffer()
{
    if (fixedCapacity)
    {
        const vk::DeviceSize bufferSize = indexCapacity * sizeof(uint32_t);
        std::tie(indexBuffer, indexBufferMemory) = rendererBackend->CreateBuffer(
            bufferSize,
            vk::BufferUsageFlagBits::eIndexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "PooledIndexBuffer (" + name + ")");
        indexBufferInfo.setBuffer(indexBuffer).setOffset(0).setRange(bufferSize);
        return;
    }
    const auto& sourceIndices = GetIndices();
    vk::DeviceSize bufferSize = sourceIndices.size() * sizeof(uint32_t);
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        usage,
        memoryPropertyFlags,
        "StagingBuffer: Index (" + name + ")");

    void *data = rendererBackend->MapMemory(stagingBufferMemory, bufferSize);
    std::memcpy(data, sourceIndices.data(), static_cast<size_t>(bufferSize));
    rendererBackend->UnmapMemory(stagingBufferMemory);

    vk::BufferUsageFlags indexBufferUsage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags indexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(indexBuffer, indexBufferMemory) = rendererBackend->CreateBuffer(
        bufferSize,
        indexBufferUsage,
        indexBufferMemoryPropertyFlags,
        "IndexBuffer (" + name + ")");

    rendererBackend->CopyBufferToBuffer(stagingBuffer, indexBuffer, bufferSize);
    rendererBackend->DestroyBuffer(stagingBuffer, stagingBufferMemory);
    
    indexBufferInfo
        .setBuffer(indexBuffer)
        .setOffset(0)
        .setRange(sourceIndices.size() * sizeof(uint32_t));
}

void RenderableObject::DestroyIndexBuffer()
{
    if (rendererBackend != nullptr)
    {
        rendererBackend->DestroyBuffer(indexBuffer, indexBufferMemory);
        indexBuffer = nullptr;
        indexBufferMemory = nullptr;
    }
}
void RenderableObject::UpdateLocalBounds()
{
    const auto& sourceVertices = GetVertices();
    if (sourceVertices.empty())
    {
        boundsMin = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        boundsMax = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        return;
    }

    boundsMin = sourceVertices[0].position;
    boundsMax = sourceVertices[0].position;
    for (const auto& vertex : sourceVertices)
    {
        boundsMin = boundsMin.cwiseMin(vertex.position);
        boundsMax = boundsMax.cwiseMax(vertex.position);
    }
}
