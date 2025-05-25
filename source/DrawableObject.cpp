#include "DrawableObject.h"

DrawableObject::DrawableObject(std::vector<float> &vertices, vk::Device *device, vk::PhysicalDeviceMemoryProperties *physicalDeviceMemoryProperties)
{
    this->device = device;
    this->vertices = &vertices;

    // 创建顶点缓冲区
    vk::BufferCreateInfo bufferCreateInfo;
    bufferCreateInfo
        .setSize(vertices.size() * sizeof(float))
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
}

DrawableObject::~DrawableObject()
{
    device->destroyBuffer(vertexBuffer);
    device->freeMemory(vertexBufferMemory);
}

void DrawableObject::Draw(vk::CommandBuffer &commandBuffer, vk::PipelineLayout &pipelineLayout, vk::Pipeline &pipeline, vk::DescriptorSet &descriptorSet)
{
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, descriptorSet, nullptr);
    commandBuffer.bindVertexBuffers(0, 1, &vertexBuffer, &vertexBufferInfo.offset);
    commandBuffer.draw(vertices->size(), 1, 0,0);
}

DrawableObject::DrawableObject()
{
}
