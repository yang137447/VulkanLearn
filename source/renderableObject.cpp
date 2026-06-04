#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "render/backend/rendererBackendVulkan.h"

#include <cstring>

RenderableObject::RenderableObject(
    std::vector<Vertex> vertices,
    std::vector<uint32_t> indices,
    VL::RendererBackendVulkan& rendererBackend,
    const std::string& name)
{
    this->rendererBackend = &rendererBackend;
    this->vertices = std::move(vertices);
    this->indices = std::move(indices);
    this->name = name;

    //更新boundingBox
    UpdateLocalBounds();
    // 创建顶点缓冲区
    CreateVertexBuffer();
    // 创建索引缓冲区
    CreateIndexBuffer();
}
RenderableObject::~RenderableObject()
{
    // 销毁索引缓冲区
    DestroyIndexBuffer();
    // 销毁顶点缓冲区
    DestroyVertexBuffer();
    rendererBackend = nullptr;
}

void RenderableObject::Draw(vk::CommandBuffer &commandBuffer, uint32_t width, uint32_t height)
{
    vk::Viewport viewport;
    viewport
        .setX(0.0f)
        .setY(0.0f)
        .setWidth(static_cast<float>(width))
        .setHeight(static_cast<float>(height))
        .setMinDepth(0.0f)
        .setMaxDepth(1.0f);
    vk::Rect2D scissor;
    scissor
        .setOffset({ 0, 0 })
        .setExtent({ 
            static_cast<uint32_t>(width), 
            static_cast<uint32_t>(height) });
    commandBuffer.setViewport(0, 1, &viewport);
    commandBuffer.setScissor(0, 1, &scissor);
    commandBuffer.bindVertexBuffers(0, 1, &vertexBuffer, &vertexBufferInfo.offset);
    commandBuffer.bindIndexBuffer(indexBuffer, 0, vk::IndexType::eUint32);
    commandBuffer.drawIndexed(indices.size(), 1, 0, 0, 0);
}

void RenderableObject::CreateVertexBuffer()
{
    vk::DeviceSize bufferSize = vertices.size() * sizeof(Vertex);
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
    std::memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
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
        .setRange(vertices.size() * sizeof(Vertex));
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
    vk::DeviceSize bufferSize = indices.size() * sizeof(uint32_t);
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
    std::memcpy(data, indices.data(), static_cast<size_t>(bufferSize));
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
        .setRange(indices.size() * sizeof(indices[0]));
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
    if (vertices.empty())
    {
        boundsMin = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        boundsMax = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
        return;
    }

    boundsMin = vertices[0].position;
    boundsMax = vertices[0].position;
    for (const auto& vertex : vertices)
    {
        boundsMin = boundsMin.cwiseMin(vertex.position);
        boundsMax = boundsMax.cwiseMax(vertex.position);
    }
}
