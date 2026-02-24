#include "renderableObject.h"
#include "vertexDataStruct.h"
#include "commonFunction.h"
#include <vulkan/vulkan.hpp>

RenderableObject::RenderableObject()
{
}

RenderableObject::RenderableObject(std::vector<Vertex> vertices, std::vector<uint32_t> indices, vk::Device *device, vk::PhysicalDeviceMemoryProperties *physicalDeviceMemoryProperties, vk::CommandPool *commandPool, vk::CommandBuffer* commandBuffer, vk::Queue *GraphicsQueue, const std::string& name)
{
    this->device = device;
    this->graphicsQueue = GraphicsQueue;
    this->physicalDeviceMemoryProperties = physicalDeviceMemoryProperties;
    this->commandPool = commandPool;
    this->commandBuffer = commandBuffer;
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
}

void RenderableObject::Draw(vk::CommandBuffer &commandBuffer, uint32_t width, uint32_t height)
{
    //commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
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
    //commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, descriptorSet, nullptr);
    commandBuffer.bindVertexBuffers(0, 1, &vertexBuffer, &vertexBufferInfo.offset);
    commandBuffer.bindIndexBuffer(indexBuffer, 0, vk::IndexType::eUint32);
    //commandBuffer.draw(vertices->size(), 1, 0,0);
    commandBuffer.drawIndexed(indices.size(), 1, 0, 0, 0);
}

void RenderableObject::CreateVertexBuffer()
{
    vk::DeviceSize bufferSize = vertices.size() * sizeof(Vertex);
    // 创建临时缓冲区
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, usage, *physicalDeviceMemoryProperties, memoryPropertyFlags, "StagingBuffer: Vertex (" + name + ")"
        );
    // 将顶点数据复制到临时缓冲区
    void *data = device->mapMemory(stagingBufferMemory, 0, bufferSize);
    memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
    device->unmapMemory(stagingBufferMemory);

    // 创建顶点缓冲区
    vk::BufferUsageFlags vertexBufferUsage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags vertexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(vertexBuffer, vertexBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, vertexBufferUsage, *physicalDeviceMemoryProperties, vertexBufferMemoryPropertyFlags, "VertexBuffer (" + name + ")"
    );

    // 将临时缓冲区中的数据复制到顶点缓冲区
    CommonFunction::CopyBufferToBuffer(*device, *graphicsQueue, *commandPool, stagingBuffer, vertexBuffer, bufferSize);

    // 释放临时缓冲区
    device->destroyBuffer(stagingBuffer);
    device->freeMemory(stagingBufferMemory);

    // 设置顶点缓冲区信息
    vertexBufferInfo
        .setBuffer(vertexBuffer)
        .setOffset(0)
        .setRange(vertices.size() * sizeof(Vertex));
}

void RenderableObject::DestroyVertexBuffer()
{
    device->destroyBuffer(vertexBuffer);
    device->freeMemory(vertexBufferMemory);
}

void RenderableObject::CreateIndexBuffer()
{
    vk::DeviceSize bufferSize = indices.size() * sizeof(uint32_t);
    // 创建临时缓冲区
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, usage, *physicalDeviceMemoryProperties, memoryPropertyFlags, "StagingBuffer: Index (" + name + ")"
    );
    // 将索引数据复制到临时缓冲区
    void *data = device->mapMemory(stagingBufferMemory, 0, bufferSize);
    memcpy(data, indices.data(), static_cast<size_t>(bufferSize));
    device->unmapMemory(stagingBufferMemory);
    // 创建索引缓冲区
    vk::BufferUsageFlags indexBufferUsage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags indexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(indexBuffer, indexBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, indexBufferUsage, *physicalDeviceMemoryProperties, indexBufferMemoryPropertyFlags, "IndexBuffer (" + name + ")"
    );
    // 将临时缓冲区中的数据复制到索引缓冲区
    CommonFunction::CopyBufferToBuffer(*device, *graphicsQueue, *commandPool, stagingBuffer, indexBuffer, bufferSize);
    // 释放临时缓冲区
    device->destroyBuffer(stagingBuffer);
    device->freeMemory(stagingBufferMemory);
    
    // 设置索引缓冲区信息
    indexBufferInfo
        .setBuffer(indexBuffer)
        .setOffset(0)
        .setRange(indices.size() * sizeof(indices[0]));
}

void RenderableObject::DestroyIndexBuffer()
{
    device->destroyBuffer(indexBuffer);
    device->freeMemory(indexBufferMemory);
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
