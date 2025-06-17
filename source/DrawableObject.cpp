#include "DrawableObject.h"
#include "VertexDataStruct.h"
#include "settings.h"

DrawableObject::DrawableObject(std::vector<Vertex> &vertices, vk::Device *device, vk::PhysicalDeviceMemoryProperties *physicalDeviceMemoryProperties)
{
    this->device = device;
    this->vertices = &vertices;

    // 创建顶点缓冲区
    vk::BufferCreateInfo bufferCreateInfo;
    bufferCreateInfo
        .setSize(vertices.size() * sizeof(Vertex))
        .setUsage(vk::BufferUsageFlagBits::eVertexBuffer)
        .setSharingMode(vk::SharingMode::eExclusive);

    vertexBuffer = device->createBuffer(bufferCreateInfo);
    assert(vertexBuffer);

    // 获取缓冲区内存要求
    vk::MemoryRequirements memoryRequirements = device->getBufferMemoryRequirements(vertexBuffer);

    // 分配内存
    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo
        .setAllocationSize(memoryRequirements.size)
        .setMemoryTypeIndex(0); // 这里需要根据实际情况设置内存类型索引
    vk::Flags<vk::MemoryPropertyFlagBits> memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    for (uint32_t i = 0; i < physicalDeviceMemoryProperties->memoryTypeCount; i++)
    {
        if ((memoryRequirements.memoryTypeBits & (1 << i)) && (physicalDeviceMemoryProperties->memoryTypes[i].propertyFlags & memoryPropertyFlags))
        {
            allocateInfo.setMemoryTypeIndex(i);
            break;
        }
    }

    vertexBufferMemory = device->allocateMemory(allocateInfo);
    assert(vertexBufferMemory);

    // 绑定缓冲区和内存
    device->bindBufferMemory(vertexBuffer, vertexBufferMemory, 0);

    // 填充数据
    void *data = device->mapMemory(vertexBufferMemory, 0, bufferCreateInfo.size);
    memcpy(data, vertices.data(), bufferCreateInfo.size);
    device->unmapMemory(vertexBufferMemory);

    vertexBufferInfo
        .setBuffer(vertexBuffer)
        .setOffset(0)
        .setRange(bufferCreateInfo.size);
    
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
    device->destroyBuffer(vertexBuffer);
    device->freeMemory(vertexBufferMemory);
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
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, descriptorSet, nullptr);
    commandBuffer.bindVertexBuffers(0, 1, &vertexBuffer, &vertexBufferInfo.offset);
    commandBuffer.draw(vertices->size(), 1, 0,0);
}

DrawableObject::DrawableObject()
{
}
