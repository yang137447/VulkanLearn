#include "DrawableObject.h"
#include "VertexDataStruct.h"
#include "settings.h"
#include "CommonFunction.h"
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>

DrawableObject::DrawableObject(std::vector<Vertex> &vertices, std::vector<uint16_t> &indices, vk::Device *device, vk::PhysicalDeviceMemoryProperties *physicalDeviceMemoryProperties, vk::CommandBuffer& commandBuffer, vk::Queue &GraphicsQueue)
{
    this->device = device;
    this->vertices = &vertices;
    this->indices = &indices;
    this->graphicsQueue = &GraphicsQueue;
    this->physicalDeviceMemoryProperties = physicalDeviceMemoryProperties;
    this->commandBuffer = &commandBuffer;

    // 创建顶点缓冲区
    CreateVertexBuffer();
    // 创建索引缓冲区
    CreateIndexBuffer();
    
    // 设置顶点输入绑定描述
    vertexInputBindingDescription
        .setBinding(0)
        .setStride(sizeof(Vertex))
        .setInputRate(vk::VertexInputRate::eVertex);

    // 设置顶点输入属性描述
    vertexInputAttributeDescriptions.resize(2);
    vertexInputAttributeDescriptions[0]
        .setBinding(0)
        .setLocation(0)
        .setFormat(vk::Format::eR32G32B32Sfloat) // 位置属性
        .setOffset(offsetof(Vertex, position)); // 位置属性在结构体中的偏移量
    vertexInputAttributeDescriptions[1]
        .setBinding(0)
        .setLocation(1)
        .setFormat(vk::Format::eR32G32Sfloat) // 颜色属性
        .setOffset(offsetof(Vertex, color)); // 颜色属性在结构体中的偏移量
}

DrawableObject::~DrawableObject()
{
    // 销毁顶点缓冲区
    DestroyVertexBuffer();
    // 销毁索引缓冲区
    DestroyIndexBuffer();
    
    
}

void DrawableObject::Draw(vk::CommandBuffer &commandBuffer, vk::PipelineLayout &pipelineLayout, vk::Pipeline &pipeline, vk::DescriptorSet &descriptorSet)
{
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
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
        .setExtent({ static_cast<uint32_t>(width), static_cast<uint32_t>(height) });
    commandBuffer.setViewport(0, 1, &viewport);
    commandBuffer.setScissor(0, 1, &scissor);
    //commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, descriptorSet, nullptr);
    commandBuffer.bindVertexBuffers(0, 1, &vertexBuffer, &vertexBufferInfo.offset);
    commandBuffer.bindIndexBuffer(indexBuffer, 0, vk::IndexType::eUint16);
    //commandBuffer.draw(vertices->size(), 1, 0,0);
    commandBuffer.drawIndexed(indices->size(), 1, 0, 0, 0);
}

DrawableObject::DrawableObject()
{
}

void DrawableObject::CreateVertexBuffer()
{
    vk::DeviceSize bufferSize = vertices->size() * sizeof(Vertex);
    // 创建临时缓冲区
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, usage, *physicalDeviceMemoryProperties, memoryPropertyFlags
        );
    // 将顶点数据复制到临时缓冲区
    void *data = device->mapMemory(stagingBufferMemory, 0, bufferSize);
    memcpy(data, vertices->data(), static_cast<size_t>(bufferSize));
    device->unmapMemory(stagingBufferMemory);

    // 创建顶点缓冲区
    vk::BufferUsageFlags vertexBufferUsage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags vertexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(vertexBuffer, vertexBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, vertexBufferUsage, *physicalDeviceMemoryProperties, vertexBufferMemoryPropertyFlags
    );

    // 将临时缓冲区中的数据复制到顶点缓冲区
    CommonFunction::CopyBufferToBuffer(*graphicsQueue, *commandBuffer, stagingBuffer, vertexBuffer, bufferSize);

    // 释放临时缓冲区
    device->destroyBuffer(stagingBuffer);
    device->freeMemory(stagingBufferMemory);

    // 设置顶点缓冲区信息
    vertexBufferInfo
        .setBuffer(vertexBuffer)
        .setOffset(0)
        .setRange(vertices->size() * sizeof(Vertex));
}

void DrawableObject::DestroyVertexBuffer()
{
    device->destroyBuffer(vertexBuffer);
    device->freeMemory(vertexBufferMemory);
}

void DrawableObject::CreateIndexBuffer()
{
    vk::DeviceSize bufferSize = indices->size() * sizeof(indices[0]);
    // 创建临时缓冲区
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, usage, *physicalDeviceMemoryProperties, memoryPropertyFlags
    );
    // 将索引数据复制到临时缓冲区
    void *data = device->mapMemory(stagingBufferMemory, 0, bufferSize);
    memcpy(data, indices->data(), static_cast<size_t>(bufferSize));
    device->unmapMemory(stagingBufferMemory);
    // 创建索引缓冲区
    vk::BufferUsageFlags indexBufferUsage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    vk::MemoryPropertyFlags indexBufferMemoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(indexBuffer, indexBufferMemory) = CommonFunction::CreateBuffer(
        *device, bufferSize, indexBufferUsage, *physicalDeviceMemoryProperties, indexBufferMemoryPropertyFlags
    );
    // 将临时缓冲区中的数据复制到索引缓冲区
    CommonFunction::CopyBufferToBuffer(*graphicsQueue, *commandBuffer, stagingBuffer, indexBuffer, bufferSize);
    // 释放临时缓冲区
    device->destroyBuffer(stagingBuffer);
    device->freeMemory(stagingBufferMemory);
    
    // 设置索引缓冲区信息
    indexBufferInfo
        .setBuffer(indexBuffer)
        .setOffset(0)
        .setRange(indices->size() * sizeof(indices[0]));
}

void DrawableObject::DestroyIndexBuffer()
{
    device->destroyBuffer(indexBuffer);
    device->freeMemory(indexBufferMemory);
}
